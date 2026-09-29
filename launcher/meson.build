project('launcher', 'c', default_options: ['c_std=gnu11', 'warning_level=2'])
# memfd_create
add_project_arguments('-D_GNU_SOURCE', language: 'c')

scanner = find_program(dependency('wayland-scanner', native: true)
  .get_variable(pkgconfig: 'wayland_scanner'))
# From Weston 15 (protocol/) plus our "thumbnail" source, for the pause screen
capture_protocol = [
  custom_target('capture-header', input: 'weston-output-capture.xml',
    output: 'weston-output-capture-client-protocol.h',
    command: [scanner, 'client-header', '@INPUT@', '@OUTPUT@']),
  custom_target('capture-code', input: 'weston-output-capture.xml',
    output: 'weston-output-capture-protocol.c',
    command: [scanner, 'private-code', '@INPUT@', '@OUTPUT@']),
]

executable('launcher', 'main.c', 'capture.c', 'nav.c', 'skyline.c', 'text.c',
  capture_protocol,
  dependencies: [
    dependency('sdl2'),
    dependency('wayland-client'),
    dependency('alsa'),
    dependency('harfbuzz'),
    dependency('fontconfig'),
    dependency('freetype2'),
    meson.get_compiler('c').find_library('m'),
  ],
  install: true)
