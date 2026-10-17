# sysd: root's side of the UI (org.senkos.System1, its bus policy), and `suspend`, which it runs
executable('sysd', 'sysd.c',
  dependencies: system1_server_dep,
  # `cmds` leaves a command's second step out; the bus' calls don't use their userdata
  c_args: ['-Wno-missing-field-initializers', '-Wno-unused-parameter'],
  install: true,
  install_dir: get_option('sbindir'))
install_data('suspend', install_dir: get_option('sbindir'), install_mode: 'rwxr-xr-x')
install_data('org.senkos.System1.conf', install_dir: get_option('datadir') / 'dbus-1' / 'system.d')
