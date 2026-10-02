project('corehost', 'c', default_options: ['c_std=gnu11', 'warning_level=2'])
add_project_arguments('-D_GNU_SOURCE', language: 'c')

# Per-core defaults under the player's own options: CORE.opt, as corehost writes them
defaults = get_option('prefix') / get_option('datadir') / 'corehost'
add_project_arguments('-DCOREHOST_DEFAULTS="@0@"'.format(defaults), language: 'c')
install_data('defaults/Mupen64Plus-Next.opt', install_dir: defaults)

executable('corehost', 'main.c',
  dependencies: [
    dependency('sdl2'),
    dependency('zlib'),
    dependency('vulkan'),
    meson.get_compiler('c').find_library('dl', required: false),
  ],
  install: true)
