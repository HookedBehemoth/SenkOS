# D-Bus interfaces as C for basu (gen.py, from their introspection XML): a header, the service's
# side and the callers'. NAME_server_dep and NAME_client_dep each.
gen = find_program('gen.py')
basu_dep = dependency('basu')
bus_inc = include_directories('.')

foreach name, xml : {
    'system1': 'org.senkos.System1.xml',    # sysd's, for the launcher
    'adapter1': 'org.bluez.Adapter1.xml',   # BlueZ's, for the launcher's Bluetooth settings
    'device1': 'org.bluez.Device1.xml',
    'agentmanager1': 'org.bluez.AgentManager1.xml',
    'agent1': 'org.bluez.Agent1.xml',       # the launcher's pairing agent
  }
  t = custom_target(name, input: xml, output: [name + '.h', name + '-server.c', name + '-client.c'],
    command: [gen, '@INPUT@', '@OUTDIR@'])
  set_variable(name + '_server_dep', declare_dependency(sources: [t[0], t[1]], include_directories: bus_inc,
    dependencies: basu_dep))
  set_variable(name + '_client_dep', declare_dependency(sources: [t[0], t[2]], include_directories: bus_inc,
    dependencies: basu_dep))
endforeach
