project('obs-vaapi', 'c')

library('obs-vaapi',
    'obs-vaapi.c',
    dependencies : [
        dependency('libobs'),
        dependency('gstreamer-1.0'),
        dependency('gstreamer-app-1.0'),
    ],
    name_prefix : '',
    install : true,
    install_dir : join_paths(get_option('libdir'), 'obs-plugins'),
)
