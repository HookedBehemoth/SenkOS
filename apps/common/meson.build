xdg_shell = dependency('wayland-protocols').get_variable(pkgconfig: 'pkgdatadir') / 'stable/xdg-shell/xdg-shell.xml'
xdg_shell_header = custom_target('xdg-shell-header', input: xdg_shell,
  output: 'xdg-shell-client-protocol.h',
  command: [scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
xdg_shell_code = custom_target('xdg-shell-code', input: xdg_shell,
  output: 'xdg-shell-protocol.c',
  command: [scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

# str alone (for apps/bus, without Wayland)
str_dep = declare_dependency(link_with: static_library('str', 'str.c'), include_directories: include_directories('.'))

# str, cfg, pad, util, xml; win: the Wayland window and the loop
common_deps = [dependency('threads'), dependency('wayland-client'), str_dep]
common_dep = declare_dependency(
  link_with: static_library('common', 'cfg.c', 'pad.c', 'win.c', 'xml.c',
    xdg_shell_header, xdg_shell_code, dependencies: common_deps),
  sources: xdg_shell_header,
  include_directories: include_directories('.'),
  dependencies: common_deps)

# xml.h's events
test('xml', executable('xml-test', 'xml_test.c', 'xml.c', build_by_default: false, dependencies: str_dep))

subdir('ui')
