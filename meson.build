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

project('obs-vaapi', 'c',
  default_options : [
    'prefix=~/.config/obs-studio/plugins/obs-vaapi/bin/64bit',
    'libdir='
  ],
)

library('obs-vaapi',
	'obs-vaapi.c',
	vcs_tag(
		command : ['git', 'describe', '--tags', '--always'],
		input : 'version.c.in',
		output : 'version.c',
	),
	dependencies : [
		dependency('libobs', version : '>=28.0.0', required : get_option('libobs')),
		dependency('gstreamer-1.0', version : '>=1.20'),
		dependency('gstreamer-app-1.0'),
		dependency('gstreamer-video-1.0'),
		dependency('libpci'),
	],
	gnu_symbol_visibility : 'hidden',
	name_prefix : '',
	install : true,
)
