#
# obs-vaapi. OBS Studio plugin.
# Copyright (C) 2022-2023 Florian Zwoch <fzwoch@gmail.com>
#
# This file is part of obs-vaapi.
#
# obs-vaapi is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 2 of the License, or
# (at your option) any later version.
#
# obs-vaapi is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with obs-vaapi. If not, see <http://www.gnu.org/licenses/>.
#

FROM debian:bookworm

RUN apt update \
 && apt install -y \
    git ninja-build meson wget dpkg-dev \
    gcc libsimde-dev libgstreamer-plugins-base1.0-dev libpci-dev \
 && rm -rf /var/lib/apt/lists/*

RUN wget https://github.com/obsproject/obs-studio/archive/refs/tags/28.0.0.tar.gz \
 && tar xvf 28.0.0.tar.gz \
 && mv obs-studio-28.0.0 obs \
 && mv obs/libobs obs/obs \
 && rm 28.0.0.tar.gz
