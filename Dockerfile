FROM debian:bullseye

RUN dpkg --add-architecture arm64 

RUN apt update \
 && apt install -y \
    git ninja-build meson wget dpkg-dev \
    gcc libsimde-dev libgstreamer-plugins-base1.0-dev \
    gcc-aarch64-linux-gnu libsimde-dev:arm64 libgstreamer-plugins-base1.0-dev:arm64 \
 && rm -rf /var/lib/apt/lists/*

RUN wget https://github.com/obsproject/obs-studio/archive/refs/tags/27.2.4.tar.gz \
 && tar xvf 27.2.4.tar.gz \
 && mv obs-studio-27.2.4 obs \
 && rm 27.2.4.tar.gz
