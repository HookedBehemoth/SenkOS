# Per-core defaults under the player's own options: CORE.opt, as corehost writes them
defaults = get_option('prefix') / get_option('datadir') / 'corehost'
install_data('defaults/Mupen64Plus-Next.opt', install_dir: defaults)

executable('corehost', 'main.c',
  c_args: '-DCOREHOST_DEFAULTS="@0@"'.format(defaults),
  dependencies: [
    common_dep, # appctl.h, pad.h, win.h
    dependency('alsa'),
    dependency('egl'),
    dependency('glesv2'), # its headers only: eglGetProcAddress
    dependency('vulkan'),
    dependency('wayland-egl'),
    dependency('zlib'),
    meson.get_compiler('c').find_library('dl', required: false),
    meson.get_compiler('c').find_library('m'),
  ],
  install: true)
