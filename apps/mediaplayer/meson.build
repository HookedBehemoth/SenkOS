xdg_shell = dependency('wayland-protocols').get_variable(pkgconfig: 'pkgdatadir') / 'stable/xdg-shell/xdg-shell.xml'
protocol = [
  custom_target('xdg-shell-header', input: xdg_shell, output: 'xdg-shell-client-protocol.h',
    command: [scanner, 'client-header', '@INPUT@', '@OUTPUT@']),
  custom_target('xdg-shell-code', input: xdg_shell, output: 'xdg-shell-protocol.c',
    command: [scanner, 'private-code', '@INPUT@', '@OUTPUT@']),
]

# The base, not the UI library: the OSD is drawn on the CPU
executable('mediaplayer', 'main.c', protocol,
  dependencies: [
    common_dep,
    dependency('gstreamer-1.0'),
    dependency('gstreamer-video-1.0'),
    dependency('gstreamer-wayland-1.0'),
    dependency('wayland-client'),
    dependency('freetype2'),
    dependency('fontconfig'),
  ],
  install: true)
