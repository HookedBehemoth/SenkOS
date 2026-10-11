# Our userspace apps, built in one go (yocto: recipes-apps/apps). common is
# what they share: the base (str, cfg, util, pad, appctl.h) and the UI
# library (common/ui).
project('apps', 'c', default_options: ['c_std=gnu11', 'warning_level=2'])
# memfd_create, getline, pthread_setname_np
add_project_arguments('-D_GNU_SOURCE', language: 'c')

scanner = find_program(dependency('wayland-scanner', native: true)
  .get_variable(pkgconfig: 'wayland_scanner'))

subdir('common')
subdir('launcher')
subdir('mediaplayer')
subdir('corehost')
subdir('downloadd')
