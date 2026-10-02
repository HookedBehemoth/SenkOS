common_dep = declare_dependency(
  link_with: static_library('common', 'cfg.c', 'pad.c', 'str.c'),
  include_directories: include_directories('.'),
  dependencies: dependency('threads'))

subdir('ui')
