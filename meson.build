#
# obs-vaapi. OBS Studio plugin.
# Copyright (C) 2022 Florian Zwoch <fzwoch@gmail.com>
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

project('obs-vaapi', 'c')

library('obs-vaapi',
	'obs-vaapi.c',
	dependencies : [
		dependency('libobs', required : get_option('gstreamer')),
		dependency('gstreamer-1.0'),
		dependency('gstreamer-app-1.0'),
	],
	gnu_symbol_visibility : 'hidden',
	name_prefix : '',
	install : true,
	install_dir : join_paths(get_option('libdir'), 'obs-plugins'),
)
