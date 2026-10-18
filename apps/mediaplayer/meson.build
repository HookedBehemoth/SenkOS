# The base, not the UI library: the OSD is drawn on the CPU
idle_inhibit_xml = dependency('wayland-protocols').get_variable(pkgconfig: 'pkgdatadir') / 'unstable/idle-inhibit/idle-inhibit-unstable-v1.xml'
idle_inhibit_client = [
  custom_target('idle-inhibit-client-header', input: idle_inhibit_xml,
    output: 'idle-inhibit-unstable-v1-client-protocol.h',
    command: [scanner, 'client-header', '@INPUT@', '@OUTPUT@']),
  custom_target('idle-inhibit-client-code', input: idle_inhibit_xml,
    output: 'idle-inhibit-unstable-v1-protocol.c',
    command: [scanner, 'private-code', '@INPUT@', '@OUTPUT@']),
]
executable('mediaplayer', 'main.c', idle_inhibit_client,
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
