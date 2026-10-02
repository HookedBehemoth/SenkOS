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

glslang = find_program('glslangValidator', native: true)
shaders = []
foreach s : [['quad.vert', 'quad_vert'], ['quad.frag', 'quad_frag']]
  shaders += custom_target(s[1], input: s[0], output: s[1] + '.h',
    command: [glslang, '-V', '--target-env', 'vulkan1.3', '--vn', s[1], '-o', '@OUTPUT@', '@INPUT@'])
endforeach

executable('launcher', 'main.c', 'apps.c', 'assets.c', 'capture.c', 'gfx.c',
  'icons.c', 'nav.c', 'romdb.c', 'running.c', 'skyline.c', 'text.c',
  capture_protocol, shaders,
  dependencies: [
    dependency('sdl2'),
    dependency('vulkan'),
    dependency('wayland-client'),
    dependency('alsa'),
    dependency('harfbuzz'),
    dependency('fontconfig'),
    dependency('freetype2'),
    dependency('libpng'),
    dependency('libturbojpeg'),
    dependency('libwebp'),
    dependency('zlib'),
    meson.get_compiler('c').find_library('m'),
  ],
  install: true)
