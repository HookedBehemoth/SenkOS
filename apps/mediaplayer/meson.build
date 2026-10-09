# The base, not the UI library: the OSD is drawn on the CPU
executable('mediaplayer', 'main.c',
  dependencies: [
    common_dep,
    dependency('gstreamer-1.0'),
    dependency('gstreamer-video-1.0'),
    dependency('gstreamer-wayland-1.0'),
    dependency('freetype2'),
    dependency('fontconfig'),
    dependency('libwebp'),
  ],
  install: true)
