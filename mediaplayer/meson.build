project('mediaplayer', 'c', default_options: ['c_std=gnu11', 'warning_level=2'])
# memfd_create
add_project_arguments('-D_GNU_SOURCE', language: 'c')

scanner = find_program(dependency('wayland-scanner', native: true)
  .get_variable(pkgconfig: 'wayland_scanner'))
xdg_shell = dependency('wayland-protocols').get_variable(pkgconfig: 'pkgdatadir') / 'stable/xdg-shell/xdg-shell.xml'
protocol = [
  custom_target('xdg-shell-header', input: xdg_shell, output: 'xdg-shell-client-protocol.h',
    command: [scanner, 'client-header', '@INPUT@', '@OUTPUT@']),
  custom_target('xdg-shell-code', input: xdg_shell, output: 'xdg-shell-protocol.c',
    command: [scanner, 'private-code', '@INPUT@', '@OUTPUT@']),
]

executable('mediaplayer', 'main.c', protocol,
  dependencies: [
    dependency('gstreamer-1.0'),
    dependency('gstreamer-video-1.0'),
    dependency('gstreamer-wayland-1.0'),
    dependency('wayland-client'),
    dependency('sdl2'),
    dependency('freetype2'),
    dependency('fontconfig'),
  ],
  install: true)
