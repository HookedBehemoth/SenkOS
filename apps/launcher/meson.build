# From Weston 15 (protocol/) plus our "thumbnail" source, for the pause screen
capture_protocol = [
  custom_target('capture-header', input: 'weston-output-capture.xml',
    output: 'weston-output-capture-client-protocol.h',
    command: [scanner, 'client-header', '@INPUT@', '@OUTPUT@']),
  custom_target('capture-code', input: 'weston-output-capture.xml',
    output: 'weston-output-capture-protocol.c',
    command: [scanner, 'private-code', '@INPUT@', '@OUTPUT@']),
]

executable('launcher', 'main.c', 'apps.c', 'capture.c', 'romdb.c', 'running.c', capture_protocol,
  dependencies: [
    common_dep,
    ui_dep,
    dependency('wayland-client'),
    dependency('alsa'),
    dependency('zlib'),
  ],
  install: true)
