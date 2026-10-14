# Weston's shell (`weston --shell=senkos`): a module, so in Weston's module dir.
# Not on a desktop without libweston.
libweston = dependency('libweston-15', required: false)
if not libweston.found()
  subdir_done()
endif
shell_protocol = [
  custom_target('shell-server-header', input: 'senkos-shell-v1.xml',
    output: 'senkos-shell-v1-server-protocol.h',
    command: [scanner, 'server-header', '@INPUT@', '@OUTPUT@']),
  custom_target('shell-server-code', input: 'senkos-shell-v1.xml',
    output: 'senkos-shell-v1-server-protocol.c',
    command: [scanner, 'private-code', '@INPUT@', '@OUTPUT@']),
]

shared_module('senkos-shell', 'shell.c', shell_protocol,
  name_prefix: '',
  dependencies: [libweston, dependency('wayland-server')],
  install: true,
  install_dir: get_option('libdir') / 'weston')
