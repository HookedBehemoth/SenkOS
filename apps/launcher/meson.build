# From Weston 15 (protocol/) plus our "thumbnail" source, for the pause screen
capture_protocol = [
  custom_target('capture-header', input: 'weston-output-capture.xml',
    output: 'weston-output-capture-client-protocol.h',
    command: [scanner, 'client-header', '@INPUT@', '@OUTPUT@']),
  custom_target('capture-code', input: 'weston-output-capture.xml',
    output: 'weston-output-capture-protocol.c',
    command: [scanner, 'private-code', '@INPUT@', '@OUTPUT@']),
]
# Ours (apps/shell): the launcher's layer and overlays, the refresh rate
shell_client_protocol = [
  custom_target('shell-client-header', input: '../shell/senkos-shell-v1.xml',
    output: 'senkos-shell-v1-client-protocol.h',
    command: [scanner, 'client-header', '@INPUT@', '@OUTPUT@']),
  custom_target('shell-client-code', input: '../shell/senkos-shell-v1.xml',
    output: 'senkos-shell-v1-protocol.c',
    command: [scanner, 'private-code', '@INPUT@', '@OUTPUT@']),
]

executable('launcher', 'main.c', 'state.c', 'home_state.c', 'pad_input.c', 'session.c', 'osd.c', 'control.c', 'input.c', 'widgets.c', 'scr_home.c', 'scr_pause.c', 'scr_licenses.c', 'scr_updates.c', 'scr_wifi.c', 'scr_storage.c', 'scr_settings.c', 'apps.c', 'capture.c', 'layer.c', 'licenses.c', 'perf.c', 'power.c', 'romdb.c', 'running.c', 'sysd.c', 'bus.c', 'bluetooth.c', 'bt_agent.c', 'scr_bluetooth.c', 'clock.c', 'keyboard.c', 'keys.c', 'updates.c', 'watch.c', 'wifi.c', capture_protocol, shell_client_protocol,
  dependencies: [
    common_dep,
    ui_dep,
    dependency('wayland-client'),
    dependency('zlib'),
    dependency('libpulse'),
    system1_client_dep,
    adapter1_client_dep,
    device1_client_dep,
    agentmanager1_client_dep,
    agent1_server_dep,
  ],
  install: true)
