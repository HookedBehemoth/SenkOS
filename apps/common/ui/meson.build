# gfx: Vulkan quads; skyline: atlas packing; ui: boxes, text, icons;
# nav: focus and scrolling; assets: images off the disk
glslang = find_program('glslangValidator', native: true)
shaders = []
foreach s : [['quad.vert', 'quad_vert'], ['quad.frag', 'quad_frag']]
  shaders += custom_target(s[1], input: s[0], output: s[1] + '.h',
    command: [glslang, '-V', '--target-env', 'vulkan1.3', '--vn', s[1], '-o', '@OUTPUT@', '@INPUT@'])
endforeach

ui_deps = [
  common_dep,
  dependency('vulkan'),
  dependency('harfbuzz'),
  dependency('fontconfig'),
  dependency('freetype2'),
  dependency('libpng'),
  dependency('libturbojpeg'),
  dependency('libwebp'),
  meson.get_compiler('c').find_library('m'),
]
ui_dep = declare_dependency(
  link_with: static_library('ui', 'assets.c', 'gfx.c', 'icons.c', 'nav.c', 'skyline.c', 'text.c', 'ui.c',
    shaders, dependencies: ui_deps),
  include_directories: include_directories('.'),
  dependencies: ui_deps)
