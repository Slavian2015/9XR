# 9XR

Docker-контейнер поднимает два виртуальных X11-дисплея: **SOURCE** (рабочий стол + приложения) и **VIEW** (окно `spherical_monitor`, которое ты видишь через noVNC). `spherical_monitor` захватывает root-окно SOURCE и рисует его как текстуру на внутренней поверхности сферы (стрелки вращают камеру, `Space` отправляет клик в центр захваченного окна).

## Запуск

```bash
    docker-compose up -d --build
```

Открыть: `http://127.0.0.1:6080/vnc.html`

Автоподключение + Local Scaling (подгоняет картинку под экран):

`http://192.168.31.100:6080/vnc.html?autoconnect=true&resize=scale`

Управление наклоном телефона (акселерометр/гироскоп → стрелки в `spherical_monitor`):

`http://192.168.31.100:6080/gyro.html?autoconnect=true&resize=scale`

Примечание: на iOS датчики часто работают только по HTTPS и после нажатия кнопки `Enable motion`.

## Переменные окружения

- `VIRT_W`, `VIRT_H` — размер виртуального экрана Xvfb (по умолчанию 5120x1440)
- `SOURCE_DISPLAY_NUM` — дисплей с рабочим столом/приложениями (по умолчанию `:0`)
- `VIEW_DISPLAY_NUM` — дисплей с `spherical_monitor` (по умолчанию `:1`)
- `VIEW_W`, `VIEW_H` — размер VIEW-дисплея (по умолчанию 1280x720)
- `CAPTURE_DISPLAY` — X11-дисплей, откуда `spherical_monitor` делает захват (по умолчанию = `SOURCE_DISPLAY_NUM`)
- `CAPTURE_FPS` — ограничение FPS захвата X11 (0/не задано = каждый кадр)
- `PANORAMA_PATH` — путь к equirectangular-панораме (PNG/JPG). Если задано, ставится как обои рабочего стола (фон), и попадает на сферу вместе с окнами приложений (например: `/assets/castle.png`).
- `VNC_PORT` (по умолчанию 5900), `NOVNC_PORT` (по умолчанию 6080)
- `VNC_PASSWORD` — если задан, включается аутентификация VNC
- `VNC_LOCALHOST_ONLY=1` — ограничить VNC слушать только localhost (noVNC продолжит работать)

## Запуск приложений на рабочем столе (SOURCE)

Примеры (окна появятся на сфере, т.к. рисуются на SOURCE):

```bash
docker exec -e DISPLAY=:0 spherical-monitor xclock
docker exec -e DISPLAY=:0 spherical-monitor xeyes
docker exec -e DISPLAY=:0 spherical-monitor glxgears
```

Если ты поменял `SOURCE_DISPLAY_NUM`, подставь его значение вместо `:0`.

## Если "всё чёрное"

- Проверь логи: `docker compose logs --tail=200 spherical-monitor`
- Если видишь предупреждение про `GL_MAX_TEXTURE_SIZE`, уменьши `VIRT_W`/`VIRT_H` (например, до 3840x1080) в `docker-compose.yml`.
- Для снижения нагрузки попробуй `CAPTURE_FPS=15`.