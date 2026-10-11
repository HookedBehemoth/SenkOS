# System updates and game packs over the network (root daemon, jobs as their own processes)
executable('downloadd', 'main.c',
  dependencies: [common_dep, dependency('libcurl'), dependency('libzstd'), dependency('libcrypto')],
  install: true)
