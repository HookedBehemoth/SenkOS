# From Weston 15 (protocol/) plus our "thumbnail" source, for the pause screen
capture_protocol = [
  custom_target('capture-header', input: 'weston-output-capture.xml',
    output: 'weston-output-capture-client-protocol.h',
    command: [scanner, 'client-header', '@INPUT@', '@OUTPUT@']),
  custom_target('capture-code', input: 'weston-output-capture.xml',
    output: 'weston-output-capture-protocol.c',
    command: [scanner, 'private-code', '@INPUT@', '@OUTPUT@']),
]

executable('launcher', 'main.c', 'state.c', 'home_state.c', 'pad_input.c', 'session.c', 'osd.c', 'control.c', 'input.c', 'widgets.c', 'scr_home.c', 'scr_pause.c', 'scr_licenses.c', 'scr_updates.c', 'scr_wifi.c', 'scr_storage.c', 'scr_settings.c', 'apps.c', 'capture.c', 'licenses.c', 'perf.c', 'power.c', 'romdb.c', 'running.c', 'sysd.c', 'clock.c', 'keyboard.c', 'keys.c', 'updates.c', 'watch.c', 'wifi.c', capture_protocol,
  dependencies: [
    common_dep,
    ui_dep,
    dependency('wayland-client'),
    dependency('zlib'),
    dependency('libpulse'),
  ],
  install: true)
