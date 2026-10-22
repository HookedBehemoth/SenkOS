# Per-core defaults under the player's own options: CORE.opt, as corehost writes them
defaults = get_option('prefix') / get_option('datadir') / 'corehost'
install_data('defaults/Mupen64Plus-Next.opt', install_dir: defaults)

# Shader presets (libretro-shaders, built by tools/slangc) and overlays (libretro-overlays)
shader_dir = get_option('prefix') / get_option('datadir') / 'corehost/shaders'
overlay_dir = get_option('prefix') / get_option('datadir') / 'corehost/overlays'

# The overlay's own quad
border_shaders = []
foreach s : [['border.vert', 'border_vert'], ['border.frag', 'border_frag']]
  border_shaders += custom_target(s[1], input: s[0], output: s[1] + '.h',
    command: [glslang, '-V', '--target-env', 'vulkan1.0', '--vn', s[1], '-o', '@OUTPUT@', '@INPUT@'])
endforeach

executable('corehost', 'main.c', 'shader.c', border_shaders,
  c_args: ['-DCOREHOST_DEFAULTS="@0@"'.format(defaults),
           '-DCOREHOST_SHADERS="@0@"'.format(shader_dir),
           '-DCOREHOST_OVERLAYS="@0@"'.format(overlay_dir)],
  dependencies: [
    common_dep, # appctl.h, pad.h, win.h
    dependency('alsa'),
    dependency('egl'),
    dependency('glesv2'), # its headers only: eglGetProcAddress
    dependency('libpng'), # the shaders' textures, the overlays
    dependency('vulkan'),
    dependency('wayland-egl'),
    dependency('zlib'),
    meson.get_compiler('c').find_library('dl', required: false),
    meson.get_compiler('c').find_library('m'),
  ],
  install: true)
