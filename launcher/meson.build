project('launcher', 'c', default_options: ['c_std=gnu11', 'warning_level=2'])

executable('launcher', 'main.c', 'nav.c', 'skyline.c', 'text.c',
  dependencies: [
    dependency('sdl2'),
    dependency('harfbuzz'),
    dependency('fontconfig'),
    dependency('freetype2'),
    meson.get_compiler('c').find_library('m'),
  ],
  install: true)
