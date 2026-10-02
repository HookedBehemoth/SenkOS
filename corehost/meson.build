project('corehost', 'c', default_options: ['c_std=gnu11', 'warning_level=2'])
add_project_arguments('-D_GNU_SOURCE', language: 'c')

executable('corehost', 'main.c',
  dependencies: [
    dependency('sdl2'),
    dependency('zlib'),
    meson.get_compiler('c').find_library('dl', required: false),
  ],
  install: true)
