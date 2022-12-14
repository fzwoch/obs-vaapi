# OBS VAAPI

[GStreamer] based VAAPI encoder implementation. Taken out of the [GStreamer OBS plugin] as a standalone plugin. Simply because the FFMPEG VAAPI implementation shows performance bottlenecks on some AMD hardware.

Supports H.264, H.265 and AV1 (WIP by upstream).

Note that not all options in the encoder properties may be working. VAAPI is just an interface and it is up to the GPU hardware and driver what is actually supported. Not all options make sense to change.

[GStreamer]: https://gstreamer.freedesktop.org/
[GStreamer OBS plugin]: https://github.com/fzwoch/obs-gstreamer/

## Quick settings TL;DR

- Set `bitrate` (e.g. `6000`).
- Set `rate-control` to `Constant bitrate`.
- Check `cabac`.

> Settings names and behavior may change depending on selected codec and installed GStreamer version.

## Install

One option is to copy the plugin to the current user's OBS plugin directory:

`~/.config/obs-studio/plugins/obs-vaapi/bin/64bit/obs-vaapi.so`

## Checklist

1. Check VAAPI is working. You are looking for `VAEntrypointEncSlice` entry points.

```
$ vainfo
libva info: VA-API version 1.16.0
libva info: Trying to open /usr/lib/x86_64-linux-gnu/dri/radeonsi_drv_video.so
libva info: Found init function __vaDriverInit_1_16
libva info: va_openDriver() returns 0
vainfo: VA-API version: 1.16 (libva 2.12.0)
vainfo: Driver version: Mesa Gallium driver 22.3.0 for AMD Radeon RX 6600 (navi23, LLVM 15.0.6, DRM 3.48, 6.0.0-6-amd64)
vainfo: Supported profile and entrypoints
      VAProfileMPEG2Simple            :	VAEntrypointVLD
      VAProfileMPEG2Main              :	VAEntrypointVLD
      VAProfileVC1Simple              :	VAEntrypointVLD
      VAProfileVC1Main                :	VAEntrypointVLD
      VAProfileVC1Advanced            :	VAEntrypointVLD
      VAProfileH264ConstrainedBaseline:	VAEntrypointVLD
      VAProfileH264ConstrainedBaseline:	VAEntrypointEncSlice    ←
      VAProfileH264Main               :	VAEntrypointVLD
      VAProfileH264Main               :	VAEntrypointEncSlice    ←
      VAProfileH264High               :	VAEntrypointVLD
      VAProfileH264High               :	VAEntrypointEncSlice    ←
      VAProfileHEVCMain               :	VAEntrypointVLD
      VAProfileHEVCMain               :	VAEntrypointEncSlice    ←
      VAProfileHEVCMain10             :	VAEntrypointVLD
      VAProfileHEVCMain10             :	VAEntrypointEncSlice    ←
      VAProfileJPEGBaseline           :	VAEntrypointVLD
      VAProfileVP9Profile0            :	VAEntrypointVLD
      VAProfileVP9Profile2            :	VAEntrypointVLD
      VAProfileAV1Profile0            :	VAEntrypointVLD
      VAProfileNone                   :	VAEntrypointVideoProc

```

2. Check GStreamer is working

```
$ gst-inspect-1.0 va
Plugin Details:
  Name                     va
  Description              VA-API codecs plugin
  Filename                 /usr/lib/x86_64-linux-gnu/gstreamer-1.0/libgstva.so
  Version                  1.20.3
  License                  LGPL
  Source module            gst-plugins-bad
  Source release date      2022-06-15
  Binary package           GStreamer Bad Plugins (Debian)
  Origin URL               http://packages.qa.debian.org/gst-plugins-bad1.0

  vadeinterlace: VA-API Deinterlacer
  vah264dec: VA-API H.264 Decoder
  vah265dec: VA-API H.265 Decoder
  vampeg2dec: VA-API Mpeg2 Decoder
  vapostproc: VA-API Video Postprocessor    ⇦
  vavp9dec: VA-API VP9 Decoder

  6 features:
  +-- 6 elements
```

```
$ gst-inspect-1.0 vaapi
Plugin Details:
  Name                     vaapi
  Description              VA-API based elements
  Filename                 /usr/lib/x86_64-linux-gnu/gstreamer-1.0/libgstvaapi.so
  Version                  1.20.3
  License                  LGPL
  Source module            gstreamer-vaapi
  Source release date      2022-06-15
  Binary package           gstreamer-vaapi
  Origin URL               Unknown package origin

  vaapidecodebin: VA-API Decode Bin
  vaapih264dec: VA-API H264 decoder
  vaapih264enc: VA-API H264 encoder    ⇦
  vaapih265dec: VA-API H265 decoder
  vaapih265enc: VA-API H265 encoder    ⇦
  vaapijpegdec: VA-API JPEG decoder
  vaapimpeg2dec: VA-API MPEG2 decoder
  vaapipostproc: VA-API video postprocessing
  vaapisink: VA-API sink
  vaapivc1dec: VA-API VC1 decoder

  10 features:
  +-- 10 elements
```

## Build

### Option #1 - Local machine build

```shell
meson --buildtype=release build
ninja -C build
```

### Option #2 - Release build

```shell
docker build . -t obs-vaapi
```

```shell
docker run --rm -v $PWD:/obs-vaapi obs-vaapi /bin/bash -c \
  "cd /obs-vaapi && meson x86_64 --buildtype=release -Dlibobs=disabled -Dc_args=-I/obs -Dc_link_args='-Wl,--unresolved-symbols=ignore-all -static-libgcc' && ninja -C x86_64"
```

#### For aarch64

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
