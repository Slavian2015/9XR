#!/usr/bin/env bash
set -euo pipefail

VIRT_W=${VIRT_W:-5120}
VIRT_H=${VIRT_H:-1440}
DISPLAY_NUM=${DISPLAY_NUM:-:0}

VNC_PORT=${VNC_PORT:-5900}
NOVNC_PORT=${NOVNC_PORT:-6080}

# Security defaults:
# - if VNC_PASSWORD is set, VNC auth is enabled
# - if not set, we still allow access (back-compat) but can be restricted with VNC_LOCALHOST_ONLY=1
VNC_PASSWORD=${VNC_PASSWORD:-}
VNC_LOCALHOST_ONLY=${VNC_LOCALHOST_ONLY:-0}

XVFB_PID=""
OPENBOX_PID=""
X11VNC_PID=""
WEBSOCKIFY_PID=""
VNC_PASSFILE=""

cleanup() {
	set +e
	[[ -n "${WEBSOCKIFY_PID}" ]] && kill "${WEBSOCKIFY_PID}" 2>/dev/null
	[[ -n "${X11VNC_PID}" ]] && kill "${X11VNC_PID}" 2>/dev/null
	[[ -n "${OPENBOX_PID}" ]] && kill "${OPENBOX_PID}" 2>/dev/null
	[[ -n "${XVFB_PID}" ]] && kill "${XVFB_PID}" 2>/dev/null

	[[ -n "${WEBSOCKIFY_PID}" ]] && wait "${WEBSOCKIFY_PID}" 2>/dev/null
	[[ -n "${X11VNC_PID}" ]] && wait "${X11VNC_PID}" 2>/dev/null
	[[ -n "${OPENBOX_PID}" ]] && wait "${OPENBOX_PID}" 2>/dev/null
	[[ -n "${XVFB_PID}" ]] && wait "${XVFB_PID}" 2>/dev/null

	[[ -n "${VNC_PASSFILE}" ]] && rm -f "${VNC_PASSFILE}" 2>/dev/null
}

trap cleanup EXIT SIGTERM SIGINT

echo "Starting Xvfb on ${DISPLAY_NUM} with ${VIRT_W}x${VIRT_H}..."
Xvfb "${DISPLAY_NUM}" -screen 0 "${VIRT_W}x${VIRT_H}x24" +extension GLX &
XVFB_PID=$!

export DISPLAY="${DISPLAY_NUM}"

echo "Waiting for X server on ${DISPLAY}..."
for i in $(seq 1 50); do
	if xdpyinfo -display "${DISPLAY}" >/dev/null 2>&1; then
		break
	fi
	sleep 0.1
done
xdpyinfo -display "${DISPLAY}" >/dev/null 2>&1 || {
	echo "X server did not become ready" >&2
	exit 1
}

openbox &
OPENBOX_PID=$!

# Optional: set a wallpaper (panorama) on the root window.
# This keeps apps like xclock visible, while giving the sphere a panoramic background.
PANORAMA_PATH=${PANORAMA_PATH:-}
if [[ -n "${PANORAMA_PATH}" ]]; then
	if [[ -f "${PANORAMA_PATH}" ]]; then
		if command -v feh >/dev/null 2>&1; then
			feh --no-fehbg --bg-fill "${PANORAMA_PATH}" || true
			echo "Wallpaper set from PANORAMA_PATH=${PANORAMA_PATH}"
		else
			echo "PANORAMA_PATH is set but 'feh' is not installed" >&2
		fi
	else
		echo "PANORAMA_PATH file not found: ${PANORAMA_PATH}" >&2
	fi
fi

X11VNC_ARGS=( -display "${DISPLAY}" -forever -shared -rfbport "${VNC_PORT}" )
if [[ -n "${VNC_PASSWORD}" ]]; then
	VNC_PASSFILE=$(mktemp)
	x11vnc -storepasswd "${VNC_PASSWORD}" "${VNC_PASSFILE}" >/dev/null
	X11VNC_ARGS+=( -rfbauth "${VNC_PASSFILE}" )
else
	# Backward compatible (insecure) default; set VNC_PASSWORD to enable auth.
	X11VNC_ARGS+=( -nopw )
fi
if [[ "${VNC_LOCALHOST_ONLY}" == "1" ]]; then
	X11VNC_ARGS+=( -localhost )
fi

x11vnc "${X11VNC_ARGS[@]}" &
X11VNC_PID=$!

websockify --web=/usr/share/novnc/ "0.0.0.0:${NOVNC_PORT}" "localhost:${VNC_PORT}" &
WEBSOCKIFY_PID=$!

echo "Starting spherical monitor..."
exec /app/spherical_monitor
