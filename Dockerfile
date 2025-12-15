# Dockerfile
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    x11-apps \
    x11-utils \
    x11-xserver-utils \
    xvfb \
    x11vnc \
    openbox \
    feh \
    tini \
    git \
    python3 \
    python3-websockify \
    novnc \
    libglfw3-dev \
    libx11-dev \
    libxtst-dev \
    libgl1-mesa-dev \
    libglu1-mesa-dev \
    build-essential \
    mesa-utils \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY spherical_monitor.cpp /app/spherical_monitor.cpp

RUN g++ spherical_monitor.cpp -o spherical_monitor \
    -lglfw -lGL -lX11 -lXtst -lm

COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

EXPOSE 6080

ENTRYPOINT ["/usr/bin/tini","--","/entrypoint.sh"]
