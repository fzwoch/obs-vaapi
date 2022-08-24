# OBS VAAPI

[GStreamer] based VAAPI encoder implementation. Taken out of the [GStreamer OBS plugin] as a standalone plugin. Simply because the FFMPEG VAAPI implementation shows performance bottlenecks on some AMD hardware.

Supports H.264 and H.265.

Note that not all options in the encoder properties may be working. VAAPI is just an interface and it is up to the GPU hardware and driver what is actually supported. Not all options make sense to change.

[GStreamer]: https://gstreamer.freedesktop.org/
[GStreamer OBS plugin]: https://github.com/fzwoch/obs-gstreamer/

# Build

## Option #1 - Local machine build

```shell
meson --buildtype=release build
ninja -C build
```

## Option #2 - Release build

```shell
docker build . -t obs-vaapi
```

```shell
docker run --rm -v $PWD:/obs-vaapi obs-vaapi /bin/bash -c \
  "cd /obs-vaapi && meson x86_64 --buildtype=release -Dlibobs=disabled -Dc_args=-I/obs -Dc_link_args='-Wl,--unresolved-symbols=ignore-all -static-libgcc' && ninja -C x86_64"
```

### For aarch64

```shell
echo "[binaries]" > arm64.txt
echo "c = 'aarch64-linux-gnu-gcc'" >> arm64.txt
echo "strip = 'aarch64-linux-gnu-strip'" >> arm64.txt
echo "pkgconfig = 'aarch64-linux-gnu-pkg-config'" >> arm64.txt
echo "" >> arm64.txt
echo "[host_machine]" >> arm64.txt
echo "system = 'linux'" >> arm64.txt
echo "cpu_family = 'aarch64'" >> arm64.txt
echo "cpu = 'aarch64'" >> arm64.txt
echo "endian = 'little'" >> arm64.txt
```

```shell
docker run --rm -v $PWD:/obs-vaapi obs-vaapi /bin/bash -c \
  "cd /obs-vaapi && meson --cross-file=arm64.txt aarch64 --buildtype=release -Dlibobs=disabled -Dc_args=-I/obs -Dc_link_args='-Wl,--unresolved-symbols=ignore-all -static-libgcc' && ninja -C aarch64
```
