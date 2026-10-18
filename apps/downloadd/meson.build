# System updates and game packs over the network (root daemon on the system bus, jobs as their own
# processes), and its bus policy
executable('downloadd', 'main.c', 'daemon.c', 'job.c',
  dependencies: [common_dep, updates1_server_dep, dependency('libcurl'), dependency('libzstd'), dependency('libcrypto')],
  install: true)
install_data('org.senkos.Updates1.conf', install_dir: get_option('datadir') / 'dbus-1' / 'system.d')
