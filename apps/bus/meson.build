# D-Bus without sd-bus: our own connection (conn.c) and wire format (wire.h, inline), and the
# interfaces as C (gen.py, from their introspection XML): a header, the service's side and the
# callers'. NAME_server_dep and NAME_client_dep each.
gen = find_program('gen.py')
bus_inc = include_directories('.')
bus_lib = static_library('bus', 'conn.c', include_directories: bus_inc)
bus_dep = declare_dependency(link_with: bus_lib, include_directories: bus_inc)

foreach name, xml : {
    'system1': 'org.senkos.System1.xml',    # sysd's, for the launcher
    'properties': 'org.freedesktop.DBus.Properties.xml',
    'adapter1': 'org.bluez.Adapter1.xml',   # BlueZ's, for the launcher's Bluetooth settings
    'device1': 'org.bluez.Device1.xml',
    'agentmanager1': 'org.bluez.AgentManager1.xml',
    'agent1': 'org.bluez.Agent1.xml',       # the launcher's pairing agent
    'wpa': 'fi.w1.wpa_supplicant1.xml',     # wpa_supplicant's, for the launcher's Wi-Fi settings
    'wpa_iface': 'fi.w1.wpa_supplicant1.Interface.xml',
    'wpa_bss': 'fi.w1.wpa_supplicant1.BSS.xml',
  }
  t = custom_target(name, input: xml, output: [name + '.h', name + '-server.c', name + '-client.c'],
    command: [gen, '@INPUT@', '@OUTDIR@'])
  set_variable(name + '_server_dep', declare_dependency(sources: [t[0], t[1]], dependencies: bus_dep))
  set_variable(name + '_client_dep', declare_dependency(sources: [t[0], t[2]], dependencies: bus_dep))
endforeach

# Encodes, decodes and talks to a real bus (the session's, when there is one)
test('bus', executable('bus-test', 'test.c', build_by_default: false, dependencies: [system1_server_dep, system1_client_dep,
  properties_client_dep, wpa_bss_client_dep], c_args: '-Wno-unused-parameter'))
