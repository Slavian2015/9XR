// spherical_monitor.cpp
#include <GLFW/glfw3.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>

#include <cmath>
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <vector>
#include <algorithm>

// ---------- глобальное состояние камеры ----------
float g_yawDeg   = 0.0f;   // вращение вокруг Y
float g_pitchDeg = 0.0f;   // вращение вокруг X

// Camera FOV (zoom). Smaller = closer, bigger = wider.
float g_fovYDeg  = 90.0f;

const float ROT_SPEED = 3.0f;   // скорость поворота стрелками

// Sphere radius used both for rendering and mouse-ray mapping.
static constexpr float SPHERE_RADIUS = 5.0f;

enum class ProjectionMode {
    Sphere,
    SphereClamp,
    Cylinder,
    Morph
};

static ProjectionMode g_projectionMode = ProjectionMode::Sphere;

// 0 = cylinder-like (less polar distortion), 1 = sphere-like.
static float g_sphericity = 1.0f;

static ProjectionMode parseProjectionModeFromEnv() {
    const char* v = std::getenv("PROJECTION_MODE");
    if (!v || std::strlen(v) == 0) return ProjectionMode::Sphere;
    if (std::strcmp(v, "sphere") == 0) return ProjectionMode::Sphere;
    if (std::strcmp(v, "sphere_clamp") == 0) return ProjectionMode::SphereClamp;
    if (std::strcmp(v, "cylinder") == 0) return ProjectionMode::Cylinder;
    if (std::strcmp(v, "morph") == 0) return ProjectionMode::Morph;
    std::cerr << "Unknown PROJECTION_MODE='" << v << "', using 'sphere'\n";
    return ProjectionMode::Sphere;
}

static const char* projectionModeName(ProjectionMode m) {
    switch (m) {
        case ProjectionMode::Sphere: return "sphere";
        case ProjectionMode::SphereClamp: return "sphere_clamp";
        case ProjectionMode::Cylinder: return "cylinder";
        case ProjectionMode::Morph: return "morph";
        default: return "sphere";
    }
}

static float parseSphericityFromEnv() {
    float s = 1.0f;
    if (const char* v = std::getenv("SPHERICITY")) {
        if (std::strlen(v) > 0) s = static_cast<float>(std::atof(v));
    }
    if (s < 0.0f) s = 0.0f;
    if (s > 1.0f) s = 1.0f;
    return s;
}

static float clamp01(float x) {
    return std::clamp(x, 0.0f, 1.0f);
}

struct Vec3 {
    float x;
    float y;
    float z;
};

static bool dirToUV_Morph(Vec3 dirWorld, float sphericity, float& outU, float& outV) {
    // Surface is a rotationally-symmetric morph between a cylinder and a sphere:
    //   r(theta) = (1-s) * 1 + s * cos(theta)
    //   y(theta) = (1-s) * theta + s * sin(theta)
    // where theta in [-pi/2, pi/2], and final position is scaled by SPHERE_RADIUS.
    // We solve for theta by intersecting the view ray with this surface in the (r,y) plane.

    const float PI = 3.14159265358979323846f;
    sphericity = clamp01(sphericity);

    float dxz = std::sqrt(dirWorld.x * dirWorld.x + dirWorld.z * dirWorld.z);
    if (dxz < 1e-6f) return false;

    float phi = std::atan2(dirWorld.z, dirWorld.x);
    if (phi < 0.0f) phi += 2.0f * PI;
    outU = phi / (2.0f * PI);

    auto f = [&](float theta) -> float {
        float r = (1.0f - sphericity) * 1.0f + sphericity * std::cos(theta);
        float y = (1.0f - sphericity) * theta + sphericity * std::sin(theta);
        // Equation: dy * r(theta) = dxz * y(theta)
        return dirWorld.y * r - dxz * y;
    };

    // Avoid exact poles where cos(theta)=0.
    float lo = -PI / 2.0f + 1e-4f;
    float hi =  PI / 2.0f - 1e-4f;
    float flo = f(lo);
    float fhi = f(hi);
    if (flo == 0.0f) {
        outV = 1.0f - ((lo + PI / 2.0f) / PI);
        return true;
    }
    if (fhi == 0.0f) {
        outV = 1.0f - ((hi + PI / 2.0f) / PI);
        return true;
    }
    if ((flo > 0.0f && fhi > 0.0f) || (flo < 0.0f && fhi < 0.0f)) {
        return false;
    }

    // Bisection.
    for (int i = 0; i < 40; ++i) {
        float mid = 0.5f * (lo + hi);
        float fmid = f(mid);
        if ((flo > 0.0f && fmid > 0.0f) || (flo < 0.0f && fmid < 0.0f)) {
            lo = mid;
            flo = fmid;
        } else {
            hi = mid;
            fhi = fmid;
        }
    }

    float theta = 0.5f * (lo + hi);
    outV = 1.0f - ((theta + PI / 2.0f) / PI);
    return true;
}

static float sphereClampThetaMaxRad() {
    // Default: 80 degrees (removes polar singularity artifacts while keeping most of the sphere).
    float deg = 80.0f;
    if (const char* s = std::getenv("SPHERE_THETA_MAX_DEG")) {
        if (std::strlen(s) > 0) {
            deg = static_cast<float>(std::atof(s));
        }
    }
    // Keep within a sane range.
    if (deg < 1.0f) deg = 1.0f;
    if (deg > 89.9f) deg = 89.9f;
    return deg * 3.14159265358979323846f / 180.0f;
}

static bool isSphereMouseEnabled() {
    const char* v = std::getenv("SPHERE_MOUSE");
    if (!v || std::strlen(v) == 0) return true;
    return std::atoi(v) != 0;
}

struct WindowCapture;

static Vec3 normalize(Vec3 v) {
    float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len <= 0.0f) return {0.0f, 0.0f, -1.0f};
    return {v.x / len, v.y / len, v.z / len};
}

static Vec3 rotateX(Vec3 v, float deg) {
    float a = deg * 3.14159265358979323846f / 180.0f;
    float c = std::cos(a);
    float s = std::sin(a);
    return {v.x, c * v.y - s * v.z, s * v.y + c * v.z};
}

static Vec3 rotateY(Vec3 v, float deg) {
    float a = deg * 3.14159265358979323846f / 180.0f;
    float c = std::cos(a);
    float s = std::sin(a);
    return {c * v.x + s * v.z, v.y, -s * v.x + c * v.z};
}

static bool viewMouseToCaptureXY(GLFWwindow* glfwWindow, const WindowCapture& cap, double xpos, double ypos, int& outX, int& outY);
static bool captureLocalToRoot(const WindowCapture& cap, int local_x, int local_y, int& root_x, int& root_y);
static void injectMouseMove(WindowCapture& cap, int local_x, int local_y);
static void injectMouseButton(WindowCapture& cap, int button, bool down);

// ---------- вспомогательные функции X11 ----------

Window findWindowByNameRecursive(Display* dpy, Window root, const char* name) {
    Window root_ret, parent_ret;
    Window* children = nullptr;
    unsigned int nchildren = 0;

    if (!XQueryTree(dpy, root, &root_ret, &parent_ret, &children, &nchildren)) {
        return 0;
    }

    Window result = 0;

    for (unsigned int i = 0; i < nchildren && result == 0; ++i) {
        Window w = children[i];

        // пытаемся получить WM_NAME
        XTextProperty prop;
        if (XGetWMName(dpy, w, &prop) && prop.value && prop.nitems) {
            std::string title(reinterpret_cast<char*>(prop.value));
            if (title.find(name) != std::string::npos) {
                result = w;
            }
            if (prop.value) {
                XFree(prop.value);
            }
        }

        if (!result) {
            result = findWindowByNameRecursive(dpy, w, name);
        }
    }

    if (children) {
        XFree(children);
    }

    return result;
}

Window getTargetWindow(Display* dpy) {
    Window root = DefaultRootWindow(dpy);

    // 1) приоритет — явный ID окна из переменной среды
    const char* idStr = std::getenv("TARGET_WINDOW_ID");
    if (idStr && std::strlen(idStr) > 0) {
        unsigned long wid = std::strtoul(idStr, nullptr, 0); // поддерживает 0x...
        if (wid != 0) {
            std::cerr << "Using window by ID: 0x" << std::hex << wid << std::dec << "\n";
            return static_cast<Window>(wid);
        }
    }

    // 2) если задано имя — ищем по части заголовка окна
    const char* name = std::getenv("TARGET_WINDOW_NAME");
    if (name && std::strlen(name) > 0) {
        std::cerr << "Searching window by name fragment: \"" << name << "\"\n";
        Window w = findWindowByNameRecursive(dpy, root, name);
        if (w) {
            std::cerr << "Found window: 0x" << std::hex << (unsigned long)w << std::dec << "\n";
            return w;
        } else {
            std::cerr << "Window with name fragment not found, fallback to root.\n";
        }
    }

    // 3) fallback — весь root (как раньше)
    std::cerr << "Using root window as source.\n";
    return root;
}

// ---------- захват окна / рабочего стола ----------

struct WindowCapture {
    Display* display = nullptr;
    Window   window  = 0;
    int      width   = 0;
    int      height  = 0;
    GLuint   texId   = 0;
    GLenum   pixelFormat   = GL_BGRA;
    GLint    internalFormat = GL_RGBA;
    int      captureFps     = 0; // 0 = as fast as render loop
    std::chrono::steady_clock::time_point lastCapture = std::chrono::steady_clock::time_point::min();
    bool     loggedFirstCapture = false;

    bool init() {
        // Capture source X server can be different from render X server (GLFW uses DISPLAY).
        // If CAPTURE_DISPLAY is set (e.g. ":0"), we capture from that display.
        const char* captureDisplayName = std::getenv("CAPTURE_DISPLAY");
        display = XOpenDisplay((captureDisplayName && std::strlen(captureDisplayName) > 0) ? captureDisplayName : nullptr);
        if (!display) {
            std::cerr << "Failed to open X display\n";
            return false;
        }

        if (captureDisplayName && std::strlen(captureDisplayName) > 0) {
            std::cerr << "Capturing from X display: " << captureDisplayName << "\n";
        }

        window = getTargetWindow(display);

        // получаем размеры окна
        XWindowAttributes attr;
        if (!XGetWindowAttributes(display, window, &attr)) {
            std::cerr << "XGetWindowAttributes failed, using default size.\n";
            width = 1024;
            height = 768;
        } else {
            width = attr.width;
            height = attr.height;
        }

        if (const char* fpsStr = std::getenv("CAPTURE_FPS")) {
            int fps = std::atoi(fpsStr);
            if (fps > 0) captureFps = fps;
        }

        // Clamp capture to GL max texture size (prevents silent GL errors on large virtual desktops).
        GLint maxTexSize = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTexSize);
        if (maxTexSize > 0 && (width > maxTexSize || height > maxTexSize)) {
            std::cerr << "WARNING: capture size " << width << "x" << height
                      << " exceeds GL_MAX_TEXTURE_SIZE=" << maxTexSize
                      << ". Clamping capture to fit. Consider lowering VIRT_W/VIRT_H.\n";
            if (width > maxTexSize) width = maxTexSize;
            if (height > maxTexSize) height = maxTexSize;
        }

        std::cerr << "Capture window size: " << width << "x" << height;
        if (captureFps > 0) {
            std::cerr << " (CAPTURE_FPS=" << captureFps << ")";
        }
        std::cerr << "\n";

        // Try to detect pixel format once.
        // Most X11 setups provide 32bpp (BGRA), but some provide 24bpp (BGR).
        if (width > 0 && height > 0) {
            XImage* probe = XGetImage(display, window, 0, 0, width, height, AllPlanes, ZPixmap);
            if (probe) {
                if (probe->bits_per_pixel == 24) {
                    pixelFormat = GL_BGR;
                    internalFormat = GL_RGB;
                } else {
                    pixelFormat = GL_BGRA;
                    internalFormat = GL_RGBA;
                }
                XDestroyImage(probe);
            }
        }

        glGenTextures(1, &texId);
        glBindTexture(GL_TEXTURE_2D, texId);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

        // Prefill with a visible pattern so you can still perceive the sphere even if the desktop is black
        // or capture temporarily fails.
        int bytesPerPixel = (pixelFormat == GL_BGR) ? 3 : 4;
        std::vector<unsigned char> fallback;
        fallback.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * static_cast<size_t>(bytesPerPixel));
        const int block = 64;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                bool on = ((x / block) % 2) ^ ((y / block) % 2);
                unsigned char v = on ? 200 : 60;
                size_t idx = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * static_cast<size_t>(bytesPerPixel);
                if (bytesPerPixel == 4) {
                    // BGRA
                    fallback[idx + 0] = v;
                    fallback[idx + 1] = v;
                    fallback[idx + 2] = v;
                    fallback[idx + 3] = 255;
                } else {
                    // BGR
                    fallback[idx + 0] = v;
                    fallback[idx + 1] = v;
                    fallback[idx + 2] = v;
                }
            }
        }

        glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height,
                     0, pixelFormat, GL_UNSIGNED_BYTE, fallback.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        return true;
    }

    void shutdown() {
        if (texId) {
            glDeleteTextures(1, &texId);
            texId = 0;
        }
        if (display) {
            XCloseDisplay(display);
            display = nullptr;
        }
    }

    void updateSizeIfChanged() {
        XWindowAttributes attr;
        if (!XGetWindowAttributes(display, window, &attr)) {
            return;
        }
        if (attr.map_state != IsViewable) {
            // For root window this should not happen, but for other windows it can.
            return;
        }
        if (attr.width != width || attr.height != height) {
            width = attr.width;
            height = attr.height;
            std::cerr << "Window size changed: " << width << "x" << height << "\n";
            glBindTexture(GL_TEXTURE_2D, texId);
            glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height,
                         0, pixelFormat, GL_UNSIGNED_BYTE, nullptr);
        }
    }

    void updateTexture() {
        if (!display) return;

        if (captureFps > 0) {
            auto now = std::chrono::steady_clock::now();
            auto minInterval = std::chrono::milliseconds(1000 / captureFps);
            if (lastCapture != std::chrono::steady_clock::time_point::min() && (now - lastCapture) < minInterval) {
                return;
            }
            lastCapture = now;
        }

        // если окно свернули/скрыли, attr.map_state может быть IsUnmapped
        updateSizeIfChanged();

        if (width <= 0 || height <= 0) return;

        XImage* img = XGetImage(display, window,
                                0, 0, width, height,
                                AllPlanes, ZPixmap);
        if (!img) {
            static auto lastLog = std::chrono::steady_clock::time_point::min();
            auto now = std::chrono::steady_clock::now();
            if (lastLog == std::chrono::steady_clock::time_point::min() || (now - lastLog) > std::chrono::seconds(2)) {
                std::cerr << "XGetImage failed\n";
                lastLog = now;
            }
            return;
        }

        if (!loggedFirstCapture) {
            std::cerr << "First successful capture (bpp=" << img->bits_per_pixel << ")\n";
            loggedFirstCapture = true;
        }

        // If format changes at runtime (rare), re-init texture.
        if (img->bits_per_pixel == 24 && pixelFormat != GL_BGR) {
            pixelFormat = GL_BGR;
            internalFormat = GL_RGB;
            glBindTexture(GL_TEXTURE_2D, texId);
            glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, pixelFormat, GL_UNSIGNED_BYTE, nullptr);
        } else if (img->bits_per_pixel != 24 && pixelFormat != GL_BGRA) {
            pixelFormat = GL_BGRA;
            internalFormat = GL_RGBA;
            glBindTexture(GL_TEXTURE_2D, texId);
            glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, pixelFormat, GL_UNSIGNED_BYTE, nullptr);
        }

        glBindTexture(GL_TEXTURE_2D, texId);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0,
                        0, 0, width, height,
                        pixelFormat, GL_UNSIGNED_BYTE,
                        img->data);

        XDestroyImage(img);
    }
};

static bool viewMouseToCaptureXY(GLFWwindow* glfwWindow, const WindowCapture& cap, double xpos, double ypos, int& outX, int& outY) {
    if (cap.width <= 0 || cap.height <= 0) return false;

    int winW = 0, winH = 0;
    int fbW = 0, fbH = 0;
    glfwGetWindowSize(glfwWindow, &winW, &winH);
    glfwGetFramebufferSize(glfwWindow, &fbW, &fbH);
    if (fbW <= 0 || fbH <= 0 || winW <= 0 || winH <= 0) return false;

    // Convert window coords -> framebuffer coords (HiDPI-safe).
    double sx = static_cast<double>(fbW) / static_cast<double>(winW);
    double sy = static_cast<double>(fbH) / static_cast<double>(winH);
    double mx = xpos * sx;
    double my = ypos * sy;

    // Normalized device coordinates.
    float ndcX = static_cast<float>((2.0 * (mx + 0.5) / static_cast<double>(fbW)) - 1.0);
    float ndcY = static_cast<float>(1.0 - (2.0 * (my + 0.5) / static_cast<double>(fbH)));

    // Reconstruct a view ray in camera space for the same projection used in rendering.
    float aspect = static_cast<float>(fbW) / static_cast<float>(fbH);
    float tanHalfFovY = std::tan(g_fovYDeg * 0.5f * 3.14159265358979323846f / 180.0f);
    Vec3 dirCam = normalize({ndcX * tanHalfFovY * aspect, ndcY * tanHalfFovY, -1.0f});

    // Convert camera-space ray to world-space direction in the sphere's model coordinates.
    Vec3 dirWorld = rotateY(rotateX(dirCam, g_pitchDeg), g_yawDeg);
    dirWorld = normalize(dirWorld);

    float u = 0.0f;
    float v = 0.0f;
    const float PI = 3.14159265358979323846f;

    if (g_projectionMode == ProjectionMode::Morph) {
        if (!dirToUV_Morph(dirWorld, g_sphericity, u, v)) return false;
    } else if (g_projectionMode == ProjectionMode::Cylinder) {
        // Intersect ray with an infinite cylinder x^2+z^2=R^2 (camera at origin).
        float dxz = std::sqrt(dirWorld.x * dirWorld.x + dirWorld.z * dirWorld.z);
        if (dxz < 1e-6f) return false;
        float t = SPHERE_RADIUS / dxz;
        float px = dirWorld.x * t;
        float py = dirWorld.y * t;
        float pz = dirWorld.z * t;

        float phi = std::atan2(pz, px); // [-pi, pi]
        if (phi < 0.0f) phi += 2.0f * PI;
        u = phi / (2.0f * PI);

        // Map cylinder height linearly to the same theta range as the sphere's equirectangular V.
        // y = R * theta, theta in [-pi/2, pi/2]
        float theta = py / SPHERE_RADIUS;
        if (theta < -PI / 2.0f || theta > PI / 2.0f) {
            return false;
        }
        v = 1.0f - ((theta + PI / 2.0f) / PI);
    } else {
        // Sphere / sphere_clamp mapping.
        float y = std::clamp(dirWorld.y, -1.0f, 1.0f);
        float theta = std::asin(y); // [-pi/2, pi/2]

        float phi = std::atan2(dirWorld.z, dirWorld.x); // [-pi, pi]
        if (phi < 0.0f) phi += 2.0f * PI;
        u = phi / (2.0f * PI);

        if (g_projectionMode == ProjectionMode::SphereClamp) {
            float tmax = sphereClampThetaMaxRad();
            if (theta < -tmax || theta > tmax) return false;
            v = 1.0f - ((theta + tmax) / (2.0f * tmax));
        } else {
            v = 1.0f - ((theta + PI / 2.0f) / PI);
        }
    }

    int cx = static_cast<int>(u * static_cast<float>(cap.width));
    int cy = static_cast<int>(v * static_cast<float>(cap.height));
    cx = std::clamp(cx, 0, std::max(0, cap.width - 1));
    cy = std::clamp(cy, 0, std::max(0, cap.height - 1));

    outX = cx;
    outY = cy;
    return true;
}

static bool captureLocalToRoot(const WindowCapture& cap, int local_x, int local_y, int& root_x, int& root_y) {
    if (!cap.display || !cap.window) return false;
    Window root = DefaultRootWindow(cap.display);
    Window child = 0;
    if (!XTranslateCoordinates(cap.display, cap.window, root, local_x, local_y, &root_x, &root_y, &child)) {
        return false;
    }
    return true;
}

static void injectMouseMove(WindowCapture& cap, int local_x, int local_y) {
    int root_x = 0, root_y = 0;
    if (!captureLocalToRoot(cap, local_x, local_y, root_x, root_y)) return;
    int screen = DefaultScreen(cap.display);
    XTestFakeMotionEvent(cap.display, screen, root_x, root_y, CurrentTime);
    XFlush(cap.display);
}

static void injectMouseButton(WindowCapture& cap, int button, bool down) {
    if (!cap.display) return;
    XTestFakeButtonEvent(cap.display, button, down ? True : False, CurrentTime);
    XFlush(cap.display);
}

// ---------- отправка клика в окно (по центру) ----------

void sendCenterClick(WindowCapture& cap) {
    if (!cap.display || !cap.window) return;

    // центр окна в его координатах
    int local_x = cap.width / 2;
    int local_y = cap.height / 2;

    // переводим в координаты root-окна
    Window root = DefaultRootWindow(cap.display);
    Window child;
    int root_x, root_y;
    int win_x = local_x;
    int win_y = local_y;

    if (!XTranslateCoordinates(cap.display,
                               cap.window, root,
                               win_x, win_y,
                               &root_x, &root_y, &child)) {
        std::cerr << "XTranslateCoordinates failed\n";
        return;
    }

    // двигаем курсор и кликаем через XTest
    int screen = DefaultScreen(cap.display);
    XTestFakeMotionEvent(cap.display, screen, root_x, root_y, CurrentTime);
    XTestFakeButtonEvent(cap.display, 1, True, CurrentTime);   // ЛКМ down
    XTestFakeButtonEvent(cap.display, 1, False, CurrentTime);  // ЛКМ up
    XFlush(cap.display);

    std::cerr << "Clicked window center at root coords: "
              << root_x << "," << root_y << "\n";
}

// ---------- отрисовка сферы с текстурой внутри ----------

void drawTexturedSphere(float radius, int rings, int sectors) {
    const float PI = 3.14159265358979323846f;

    for (int r = 0; r < rings; ++r) {
        float v1 = (float)r / (float)rings;
        float v2 = (float)(r + 1) / (float)rings;

        float theta1 = v1 * PI - PI / 2.0f;      // -pi/2 .. pi/2
        float theta2 = v2 * PI - PI / 2.0f;

        glBegin(GL_QUAD_STRIP);
        for (int s = 0; s <= sectors; ++s) {
            float u = (float)s / (float)sectors;
            float phi = u * 2.0f * PI;           // 0..2pi

            float x1 = radius * std::cos(theta1) * std::cos(phi);
            float y1 = radius * std::sin(theta1);
            float z1 = radius * std::cos(theta1) * std::sin(phi);

            float x2 = radius * std::cos(theta2) * std::cos(phi);
            float y2 = radius * std::sin(theta2);
            float z2 = radius * std::cos(theta2) * std::sin(phi);

            float u1 = phi / (2.0f * PI);
            float v1_tex = 1.0f - ((theta1 + PI/2.0f) / PI);

            float u2 = phi / (2.0f * PI);
            float v2_tex = 1.0f - ((theta2 + PI/2.0f) / PI);

            glTexCoord2f(u1, v1_tex);
            glVertex3f(x1, y1, z1);

            glTexCoord2f(u2, v2_tex);
            glVertex3f(x2, y2, z2);
        }
        glEnd();
    }
}

void drawTexturedSphereClamped(float radius, float thetaMaxRad, int rings, int sectors) {
    const float PI = 3.14159265358979323846f;
    thetaMaxRad = std::clamp(thetaMaxRad, 0.01f, (PI / 2.0f) - 0.001f);

    for (int r = 0; r < rings; ++r) {
        float v1 = (float)r / (float)rings;
        float v2 = (float)(r + 1) / (float)rings;

        float theta1 = -thetaMaxRad + v1 * (2.0f * thetaMaxRad);
        float theta2 = -thetaMaxRad + v2 * (2.0f * thetaMaxRad);

        glBegin(GL_QUAD_STRIP);
        for (int s = 0; s <= sectors; ++s) {
            float u = (float)s / (float)sectors;
            float phi = u * 2.0f * PI;

            float x1 = radius * std::cos(theta1) * std::cos(phi);
            float y1 = radius * std::sin(theta1);
            float z1 = radius * std::cos(theta1) * std::sin(phi);

            float x2 = radius * std::cos(theta2) * std::cos(phi);
            float y2 = radius * std::sin(theta2);
            float z2 = radius * std::cos(theta2) * std::sin(phi);

            float u1 = phi / (2.0f * PI);
            float v1_tex = 1.0f - ((theta1 + thetaMaxRad) / (2.0f * thetaMaxRad));
            float u2 = phi / (2.0f * PI);
            float v2_tex = 1.0f - ((theta2 + thetaMaxRad) / (2.0f * thetaMaxRad));

            glTexCoord2f(u1, v1_tex);
            glVertex3f(x1, y1, z1);

            glTexCoord2f(u2, v2_tex);
            glVertex3f(x2, y2, z2);
        }
        glEnd();
    }
}

void drawTexturedCylinder(float radius, int rings, int sectors) {
    const float PI = 3.14159265358979323846f;

    // Match sphere equirectangular V range: theta in [-pi/2, pi/2] and y = R * theta.
    float yMin = -radius * (PI / 2.0f);
    float yMax =  radius * (PI / 2.0f);

    for (int r = 0; r < rings; ++r) {
        float v1 = (float)r / (float)rings;
        float v2 = (float)(r + 1) / (float)rings;

        float y1 = yMax + (yMin - yMax) * v1; // top -> bottom
        float y2 = yMax + (yMin - yMax) * v2;

        glBegin(GL_QUAD_STRIP);
        for (int s = 0; s <= sectors; ++s) {
            float u = (float)s / (float)sectors;
            float phi = u * 2.0f * PI;

            float x = radius * std::cos(phi);
            float z = radius * std::sin(phi);

            glTexCoord2f(u, v1);
            glVertex3f(x, y1, z);

            glTexCoord2f(u, v2);
            glVertex3f(x, y2, z);
        }
        glEnd();
    }
}

void drawTexturedMorph(float radius, float sphericity, int rings, int sectors) {
    const float PI = 3.14159265358979323846f;
    sphericity = clamp01(sphericity);

    for (int r = 0; r < rings; ++r) {
        float v1 = (float)r / (float)rings;
        float v2 = (float)(r + 1) / (float)rings;

        float theta1 = v1 * PI - PI / 2.0f;
        float theta2 = v2 * PI - PI / 2.0f;

        glBegin(GL_QUAD_STRIP);
        for (int s = 0; s <= sectors; ++s) {
            float u = (float)s / (float)sectors;
            float phi = u * 2.0f * PI;

            // Sphere positions.
            float sx1 = radius * std::cos(theta1) * std::cos(phi);
            float sy1 = radius * std::sin(theta1);
            float sz1 = radius * std::cos(theta1) * std::sin(phi);
            float sx2 = radius * std::cos(theta2) * std::cos(phi);
            float sy2 = radius * std::sin(theta2);
            float sz2 = radius * std::cos(theta2) * std::sin(phi);

            // Cylinder positions (same phi, theta range mapped to y).
            float cx = radius * std::cos(phi);
            float cz = radius * std::sin(phi);
            float cy1 = radius * theta1;
            float cy2 = radius * theta2;

            // Morph between cylinder and sphere.
            float x1 = (1.0f - sphericity) * cx + sphericity * sx1;
            float y1 = (1.0f - sphericity) * cy1 + sphericity * sy1;
            float z1 = (1.0f - sphericity) * cz + sphericity * sz1;
            float x2 = (1.0f - sphericity) * cx + sphericity * sx2;
            float y2 = (1.0f - sphericity) * cy2 + sphericity * sy2;
            float z2 = (1.0f - sphericity) * cz + sphericity * sz2;

            // Keep the same equirectangular UV mapping as the full sphere.
            float u1 = phi / (2.0f * PI);
            float v1_tex = 1.0f - ((theta1 + PI/2.0f) / PI);
            float u2 = phi / (2.0f * PI);
            float v2_tex = 1.0f - ((theta2 + PI/2.0f) / PI);

            glTexCoord2f(u1, v1_tex);
            glVertex3f(x1, y1, z1);

            glTexCoord2f(u2, v2_tex);
            glVertex3f(x2, y2, z2);
        }
        glEnd();
    }
}

static double g_lastCursorX = 0.0;
static double g_lastCursorY = 0.0;
static bool g_leftMouseDown = false;

static void onCursorPos(GLFWwindow* w, double xpos, double ypos) {
    g_lastCursorX = xpos;
    g_lastCursorY = ypos;

    if (!isSphereMouseEnabled()) return;
    if (!g_leftMouseDown) return;

    auto* cap = static_cast<WindowCapture*>(glfwGetWindowUserPointer(w));
    if (!cap) return;

    int cx = 0, cy = 0;
    if (!viewMouseToCaptureXY(w, *cap, xpos, ypos, cx, cy)) return;
    injectMouseMove(*cap, cx, cy);
}

static void onMouseButton(GLFWwindow* w, int button, int action, int /*mods*/) {
    if (!isSphereMouseEnabled()) return;

    if (button != GLFW_MOUSE_BUTTON_LEFT) return;

    auto* cap = static_cast<WindowCapture*>(glfwGetWindowUserPointer(w));
    if (!cap) return;

    double xpos = 0.0, ypos = 0.0;
    glfwGetCursorPos(w, &xpos, &ypos);
    g_lastCursorX = xpos;
    g_lastCursorY = ypos;

    int cx = 0, cy = 0;
    if (!viewMouseToCaptureXY(w, *cap, xpos, ypos, cx, cy)) {
        // Still update button state to avoid getting stuck.
        if (action == GLFW_PRESS) g_leftMouseDown = true;
        if (action == GLFW_RELEASE) g_leftMouseDown = false;
        return;
    }

    // Move pointer to the mapped location before clicking.
    injectMouseMove(*cap, cx, cy);

    if (action == GLFW_PRESS) {
        g_leftMouseDown = true;
        injectMouseButton(*cap, 1, true);
    } else if (action == GLFW_RELEASE) {
        g_leftMouseDown = false;
        injectMouseButton(*cap, 1, false);
    }
}

int main() {
    if (!glfwInit()) {
        std::cerr << "Failed to init GLFW\n";
        return 1;
    }

    g_projectionMode = parseProjectionModeFromEnv();
    g_sphericity = parseSphericityFromEnv();
    std::cerr << "Projection mode: " << projectionModeName(g_projectionMode);
    if (g_projectionMode == ProjectionMode::SphereClamp) {
        std::cerr << " (SPHERE_THETA_MAX_DEG=" << (sphereClampThetaMaxRad() * 180.0f / 3.14159265358979323846f) << ")";
    }
    if (g_projectionMode == ProjectionMode::Morph) {
        std::cerr << " (SPHERICITY=" << g_sphericity << ")";
    }
    std::cerr << "\n";

    GLFWwindow* window = glfwCreateWindow(1280, 720,
                                          "Spherical Monitor (Window Capture)",
                                          nullptr, nullptr);
    if (!window) {
        std::cerr << "Failed to create GLFW window\n";
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    glEnable(GL_TEXTURE_2D);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);

    WindowCapture cap;
    if (!cap.init()) {
        std::cerr << "Window capture init failed\n";
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    glfwSetWindowUserPointer(window, &cap);
    glfwSetCursorPosCallback(window, onCursorPos);
    glfwSetMouseButtonCallback(window, onMouseButton);

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // управление камерой стрелками
        if (glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS) {
            g_yawDeg += ROT_SPEED;
        }
        if (glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS) {
            g_yawDeg -= ROT_SPEED;
        }
        if (glfwGetKey(window, GLFW_KEY_UP) == GLFW_PRESS) {
            g_pitchDeg += ROT_SPEED;
            if (g_pitchDeg > 89.0f) g_pitchDeg = 89.0f;
        }
        if (glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_PRESS) {
            g_pitchDeg -= ROT_SPEED;
            if (g_pitchDeg < -89.0f) g_pitchDeg = -89.0f;
        }

        // Space — клик по центру захваченного окна
        static bool spaceWasDown = false;
        bool spaceDown = (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS);
        if (spaceDown && !spaceWasDown) {
            sendCenterClick(cap);
        }
        spaceWasDown = spaceDown;

        // W/S — adjust sphericity (more/less spherical). Switches to morph mode.
        static bool wWasDown = false;
        static bool sWasDown = false;
        bool wDown = (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS);
        bool sDown = (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS);
        if (wDown && !wWasDown) {
            g_projectionMode = ProjectionMode::Morph;
            g_sphericity = clamp01(g_sphericity + 0.1f);
            std::cerr << "Morph sphericity: " << g_sphericity << "\n";
        }
        if (sDown && !sWasDown) {
            g_projectionMode = ProjectionMode::Morph;
            g_sphericity = clamp01(g_sphericity - 0.1f);
            std::cerr << "Morph sphericity: " << g_sphericity << "\n";
        }
        wWasDown = wDown;
        sWasDown = sDown;

        // Q/E — zoom in/out (changes FOV).
        static bool qWasDown = false;
        static bool eWasDown = false;
        bool qDown = (glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS);
        bool eDown = (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS);
        if (qDown && !qWasDown) {
            g_fovYDeg -= 5.0f;
            if (g_fovYDeg < 30.0f) g_fovYDeg = 30.0f;
            std::cerr << "FOV: " << g_fovYDeg << "\n";
        }
        if (eDown && !eWasDown) {
            g_fovYDeg += 5.0f;
            if (g_fovYDeg > 120.0f) g_fovYDeg = 120.0f;
            std::cerr << "FOV: " << g_fovYDeg << "\n";
        }
        qWasDown = qDown;
        eWasDown = eDown;

        // P — cycle projection modes at runtime.
        static bool pWasDown = false;
        bool pDown = (glfwGetKey(window, GLFW_KEY_P) == GLFW_PRESS);
        if (pDown && !pWasDown) {
            if (g_projectionMode == ProjectionMode::Sphere) {
                g_projectionMode = ProjectionMode::SphereClamp;
            } else if (g_projectionMode == ProjectionMode::SphereClamp) {
                g_projectionMode = ProjectionMode::Cylinder;
            } else if (g_projectionMode == ProjectionMode::Cylinder) {
                g_projectionMode = ProjectionMode::Morph;
            } else {
                g_projectionMode = ProjectionMode::Sphere;
            }
            std::cerr << "Projection mode switched to: " << projectionModeName(g_projectionMode);
            if (g_projectionMode == ProjectionMode::SphereClamp) {
                std::cerr << " (SPHERE_THETA_MAX_DEG=" << (sphereClampThetaMaxRad() * 180.0f / 3.14159265358979323846f) << ")";
            }
            if (g_projectionMode == ProjectionMode::Morph) {
                std::cerr << " (SPHERICITY=" << g_sphericity << ")";
            }
            std::cerr << "\n";
        }
        pWasDown = pDown;

        // обновляем текстуру окна
        cap.updateTexture();

        int winW, winH;
        glfwGetFramebufferSize(window, &winW, &winH);
        glViewport(0, 0, winW, winH);

        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // проекция
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        float aspect = (float)winW / (float)winH;
        float fovY = g_fovYDeg;
        float fH = std::tan(fovY / 360.0f * 3.14159265f) * 0.1f;
        float fW = fH * aspect;
        glFrustum(-fW, fW, -fH, fH, 0.1f, 100.0f);

        // камера
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glRotatef(-g_pitchDeg, 1.0f, 0.0f, 0.0f);
        glRotatef(-g_yawDeg,   0.0f, 1.0f, 0.0f);

        // рисуем сферу с текстурой захваченного окна
        glBindTexture(GL_TEXTURE_2D, cap.texId);
        if (g_projectionMode == ProjectionMode::Morph) {
            drawTexturedMorph(SPHERE_RADIUS, g_sphericity, 64, 128);
        } else if (g_projectionMode == ProjectionMode::Cylinder) {
            drawTexturedCylinder(SPHERE_RADIUS, 64, 128);
        } else if (g_projectionMode == ProjectionMode::SphereClamp) {
            drawTexturedSphereClamped(SPHERE_RADIUS, sphereClampThetaMaxRad(), 64, 128);
        } else {
            drawTexturedSphere(SPHERE_RADIUS, 64, 128);
        }

        glfwSwapBuffers(window);
    }

    cap.shutdown();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
