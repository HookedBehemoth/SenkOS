project('bindless', 'c', default_options: ['c_std=gnu11', 'warning_level=2'])

glslang = find_program('glslangValidator', native: true)
shaders = []
foreach s : [['rect.vert', 'rect_vert'], ['rect.frag', 'rect_frag']]
  shaders += custom_target(s[1], input: s[0], output: s[1] + '.h',
    command: [glslang, '-V', '--target-env', 'vulkan1.3', '--vn', s[1], '-o', '@OUTPUT@', '@INPUT@'])
endforeach

executable('bindless-vk', 'main.c', shaders,
  dependencies: [dependency('sdl2'), dependency('vulkan'), meson.get_compiler('c').find_library('m')],
  install: true)
