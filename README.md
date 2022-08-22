# OBS VAAPI

[GStreamer] based VAAPI encoder implementation. Taken out of the [GStreamer OBS plugin] as a standalone plugin. Simply because the FFMPEG VAAPI implementation shows performance bottlenecks on some AMD hardware.

Supports H.264 and H.265.

Note that not all options in the encoder properties may be working. VAAPI is just an interface and it is up to the GPU hardware and driver what is actually supported. Not all options make sense to change.

[GStreamer]: https://gstreamer.freedesktop.org/
[GStreamer OBS plugin]: https://github.com/fzwoch/obs-gstreamer/

# Build

## Option #1

```shell
meson --buildtype=release build
ninja -C build
```

## Option #2

```shell
docker . -t obs-vaapi
docker run --rm -v $PWD:/obs-vaapi --rm obs-vaapi \
  /bin/bash -c "cd /obs-vaapi && meson build --buildtype=release -Dlibobs=disabled -Dc_args=-I/obs -Dc_link_args='-Wl,--unresolved-symbols=ignore-all -static-libgcc' && ninja -C build"
```
