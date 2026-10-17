#!/usr/bin/env python3
"""
A D-Bus interface as C for basu (sd-bus), from its introspection XML:

    gen.py INTERFACE.xml OUTDIR

writes NAME.h, NAME-server.c and NAME-client.c, NAME the interface's last element in lower case
(org.senkos.System1: system1). The XML is one <node name="/path"> with one <interface>: the
service owns the interface's name on the bus and serves it at the node's path. Another service's
(org.bluez.Device1): <annotation name="org.senkos.Service" value="org.bluez"/> in the interface;
a node without a name, and each call takes the object's path. <annotation name="org.senkos.Prefix"
value="wpa_iface"/> names it otherwise. A comment right before a method, property or signal is its
documentation, copied into NAME.h.

Server (NAME-server.c): NAME_vtable, for sd_bus_add_object_vtable. A call runs the service's

    int NAME_METHOD(sd_bus_message *call, void *userdata, IN...);

which replies now or later (holding the call: sd_bus_message_ref), with NAME_METHOD_reply(call,
OUT...) or an error; a negative errno replies with that error. A string argument is the call's:
keep the call or copy it.

Client (NAME-client.c), nothing allocated per call:

    int NAME_call_METHOD(sd_bus *bus, [const char *path,] IN..., const NAME_METHOD_then *then);

`then` (or NULL: no answer wanted) is the caller's until the answer, {done, userdata}: done runs
once from sd_bus_process with the error (NULL: none) and OUT... (zero after an error).

Properties (<property>, basic types and ay; strings and byte arrays cut at 64 bytes, an ay's
length in PROP_len): NAME_props, NAME_props_apply for an a{sv} (GetManagedObjects,
InterfacesAdded, PropertiesChanged: the keys it has, a NAME_PROP_* bit each that changed),
NAME_set_PROP for the writable ones, and NAME_watch_properties for the PropertiesChanged signals.

Signals (<signal>, basic types, an a{sv} last): NAME_watch_SIGNAL(bus, fn), fn(object, ARGS...)
with the a{sv} still to read (NAME_props_apply).

Methods' types: basic ones alone so far.
"""
import re
import sys
import xml.etree.ElementTree as ET

# D-Bus type: C type
TYPES = {'b': 'bool', 'y': 'uint8_t', 'n': 'int16_t', 'q': 'uint16_t', 'i': 'int32_t',
         'u': 'uint32_t', 'x': 'int64_t', 't': 'uint64_t', 'd': 'double',
         's': 'const char *', 'o': 'const char *', 'g': 'const char *'}
STRING = 64


def fail(why):
    sys.exit(f'gen.py: {why}')


def snake(name):
    return re.sub(r'(?<=[a-z0-9])(?=[A-Z])|(?<=[A-Z])(?=[A-Z][a-z])', '_', name).lower()


def ctype(t):
    if t not in TYPES:
        fail(f'type {t}: only basic types so far')
    return TYPES[t]


def var(t, n):
    c = ctype(t)
    return f'{c}{"" if c.endswith("*") else " "}{n}'


def decl(args):
    return ''.join(f', {var(t, n)}' for t, n in args)


def read_vars(args):
    """Locals to read `args` into (a bool is read as an int), and their addresses"""
    lines = [f'  {"int" if t == "b" else ctype(t)}{"" if ctype(t).endswith("*") else " "}{n} = {"NULL" if ctype(t).endswith("*") else "0"};'
             for t, n in args]
    return lines, ', '.join(f'&{n}' for _, n in args)


def main():
    if len(sys.argv) != 3:
        fail('usage: gen.py INTERFACE.xml OUTDIR')
    root = ET.parse(sys.argv[1], ET.XMLParser(target=ET.TreeBuilder(insert_comments=True))).getroot()
    path = root.get('name')
    ifaces = root.findall('interface')
    if root.tag != 'node' or len(ifaces) != 1:
        fail('one <node> with one <interface>')
    iface = ifaces[0].get('name')
    prefix = iface.rsplit('.', 1)[-1].lower()
    upper = prefix.upper()
    service = iface

    methods, props, signals, doc = [], [], [], None
    for el in ifaces[0]:
        if el.tag is ET.Comment:
            doc = ' '.join(el.text.split())
            continue
        if el.tag == 'annotation' and el.get('name') == 'org.senkos.Service':
            service = el.get('value')
        elif el.tag == 'annotation' and el.get('name') == 'org.senkos.Prefix':
            prefix = el.get('value')
            upper = prefix.upper()
        elif el.tag == 'method':
            args = {'in': [], 'out': []}
            for a in el.findall('arg'):
                args[a.get('direction', 'in')].append((a.get('type'), a.get('name') or f'arg{len(args["in"]) + len(args["out"])}'))
            methods.append((el.get('name'), snake(el.get('name')), args['in'], args['out'], doc))
        elif el.tag == 'property':
            if el.get('type') != 'ay':
                ctype(el.get('type'))
            props.append((el.get('name'), snake(el.get('name')), el.get('type'), 'write' in el.get('access', ''), doc))
        elif el.tag == 'signal':
            args = [(a.get('type'), a.get('name') or f'arg{i}') for i, a in enumerate(el.findall('arg'))]
            for i, (t, _) in enumerate(args):
                if t != 'a{sv}' or i != len(args) - 1:
                    ctype(t)
            signals.append((el.get('name'), snake(el.get('name')), args, doc))
        else:
            fail(f'<{el.tag}>: methods, properties and signals alone so far')
        doc = None

    out = sys.argv[2]
    src = sys.argv[1].rsplit('/', 1)[-1]
    head = f'/* Generated by apps/bus/gen.py from {src}: not to be edited */\n'
    at = '' if path is not None else ', const char *path'
    at_arg = 'path' if path is None else upper + '_PATH'

    h = [head, '#pragma once', '', '#include <basu/sd-bus.h>', '#include <stdbool.h>', '#include <stdint.h>', '',
         f'#define {upper}_NAME "{iface}"', f'#define {upper}_SERVICE "{service}"']
    if path is not None:
        h.append(f'#define {upper}_PATH "{path}"')
    h += ['', '/* The service\'s */', f'extern const sd_bus_vtable {prefix}_vtable[];']
    for name, sn, ins, outs, d in methods:
        h.append('')
        if d:
            h.append(f'/* {d} */')
        h.append(f'int {prefix}_{sn}(sd_bus_message *call, void *userdata{decl(ins)});')
        if outs:
            h.append(f'int {prefix}_{sn}_reply(sd_bus_message *call{decl(outs)});')
    h += ['', '/* The client\'s */']
    for name, sn, ins, outs, _ in methods:
        h.append(f'typedef void {prefix}_{sn}_done(const sd_bus_error *error{decl(outs)}, void *userdata);')
        h.append(f'typedef struct {{ {prefix}_{sn}_done *done; void *userdata; }} {prefix}_{sn}_then;')
        h.append(f'int {prefix}_call_{sn}(sd_bus *bus{at}{decl(ins)}, const {prefix}_{sn}_then *then);')
    if props:
        h += ['', '/* Its properties as last told */', 'typedef struct {']
        for name, sn, t, _, d in props:
            field = (f'uint8_t {sn}[{STRING}]; uint8_t {sn}_len' if t == 'ay'
                     else f'char {sn}[{STRING}]' if ctype(t).endswith('*') else var(t, sn))
            h.append(f'  {field};{f" /* {d} */" if d else ""}')
        h += [f'}} {prefix}_props;', 'enum {']
        h += [f'  {upper}_PROP_{sn.upper()} = 1u << {i},' for i, (_, sn, _, _, _) in enumerate(props)]
        h += ['};',
              f'/* An a{{sv}}, read through: the keys it has; in *changed (or NULL), a {upper}_PROP_* bit each',
              f' * that changed */',
              f'int {prefix}_props_apply({prefix}_props *p, sd_bus_message *m, uint32_t *changed);',
              f'/* PropertiesChanged of {iface}\'s objects: `changed` with the a{{sv}} for {prefix}_props_apply */',
              f'typedef void {prefix}_changed(const char *path, sd_bus_message *props);',
              f'int {prefix}_watch_properties(sd_bus *bus, {prefix}_changed *changed);']
        if any(w for *_, w, _ in props):
            h += [f'typedef void {prefix}_set_done(const sd_bus_error *error, void *userdata);',
                  f'typedef struct {{ {prefix}_set_done *done; void *userdata; }} {prefix}_set_then;']
            for name, sn, t, w, _ in props:
                if w and t == 'ay':
                    fail(f'{name}: writable byte arrays not so far')
                if w:
                    h.append(f'int {prefix}_set_{sn}(sd_bus *bus{at}, {var(t, "value")}, const {prefix}_set_then *then);')

    for name, sn, args, d in signals:
        h.append('')
        if d:
            h.append(f'/* {d} */')
        params = ''.join(', sd_bus_message *props' if t == 'a{sv}' else f', {var(t, n)}' for t, n in args)
        h.append(f'typedef void {prefix}_{sn}_fn(const char *object{params});')
        h.append(f'int {prefix}_watch_{sn}(sd_bus *bus, {prefix}_{sn}_fn *fn);')

    s = [head, f'#include "{prefix}.h"', '']
    for name, sn, ins, outs, _ in methods:
        sig = ''.join(t for t, _ in ins)
        s.append(f'static int on_{sn}(sd_bus_message *m, void *userdata, sd_bus_error *error) {{')
        s.append('  (void)error;')
        if ins:
            lines, addrs = read_vars(ins)
            s += lines
            s.append(f'  const int r = sd_bus_message_read(m, "{sig}", {addrs});')
            s.append(f'  return r < 0 ? r : {prefix}_{sn}(m, userdata, {", ".join(n for _, n in ins)});')
        else:
            s.append(f'  return {prefix}_{sn}(m, userdata);')
        s += ['}', '']
        if outs:
            s.append(f'int {prefix}_{sn}_reply(sd_bus_message *call{decl(outs)}) {{')
            s.append(f'  return sd_bus_reply_method_return(call, "{"".join(t for t, _ in outs)}", {", ".join(n for _, n in outs)});')
            s += ['}', '']
    s.append(f'const sd_bus_vtable {prefix}_vtable[] = {{')
    s.append('    SD_BUS_VTABLE_START(0),')
    s.append('    /* Who may call is the bus\' policy\'s say */')
    for name, sn, ins, outs, _ in methods:
        s.append(f'    SD_BUS_METHOD("{name}", "{"".join(t for t, _ in ins)}", "{"".join(t for t, _ in outs)}", on_{sn}, SD_BUS_VTABLE_UNPRIVILEGED),')
    s += ['    SD_BUS_VTABLE_END,', '};']

    c = [head, '#include <stdio.h>', '#include <string.h>', '', f'#include "{prefix}.h"', '']
    if any(outs for _, _, _, outs, _ in methods):
        c += ['static const sd_bus_error malformed = SD_BUS_ERROR_MAKE_CONST(SD_BUS_ERROR_INVALID_ARGS, "a reply of other types");', '']
    for name, sn, ins, outs, _ in methods:
        c.append(f'static int {sn}_replied(sd_bus_message *m, void *userdata, sd_bus_error *error) {{')
        c += ['  (void)error;', f'  const {prefix}_{sn}_then *then = userdata;',
              '  const sd_bus_error *e = sd_bus_message_get_error(m);']
        if outs:
            lines, addrs = read_vars(outs)
            c += lines
            c.append(f'  if (e == NULL && sd_bus_message_read(m, "{"".join(t for t, _ in outs)}", {addrs}) < 0)')
            c.append('    e = &malformed;')
        c.append(f'  then->done(e, {"".join(n + ", " for _, n in outs)}then->userdata);')
        c += ['  return 0;', '}', '']
        c.append(f'int {prefix}_call_{sn}(sd_bus *bus{at}{decl(ins)}, const {prefix}_{sn}_then *then) {{')
        sig = ''.join(t for t, _ in ins)
        args = ''.join(f', {"(int)" if t == "b" else ""}{n}' for t, n in ins)
        c.append(f'  return sd_bus_call_method_async(bus, NULL, {upper}_SERVICE, {at_arg}, {upper}_NAME, "{name}", '
                 f'then != NULL ? {sn}_replied : NULL, (void *)then, {f"{chr(34)}{sig}{chr(34)}" if sig else "NULL"}{args});')
        c += ['}', '']
    if props:
        c.append(f'int {prefix}_props_apply({prefix}_props *p, sd_bus_message *m, uint32_t *changed) {{')
        c += ['  uint32_t bits = 0;', '  int r = sd_bus_message_enter_container(m, \'a\', "{sv}");',
              '  while (r >= 0 && (r = sd_bus_message_enter_container(m, \'e\', "sv")) > 0) {',
              '    const char *key, *type;', '    r = sd_bus_message_read(m, "s", &key);',
              '    if (r >= 0)', '      r = sd_bus_message_peek_type(m, NULL, &type);', '    if (r < 0)', '      break;']
        first = True
        for name, sn, t, _, _ in props:
            c.append(f'    {"" if first else "} else "}if (strcmp(key, "{name}") == 0 && strcmp(type, "{t}") == 0) {{')
            first = False
            if t == 'ay':
                c += ['      const void *v = NULL;', '      size_t n = 0;', '      r = sd_bus_message_enter_container(m, \'v\', "ay");',
                      '      if (r >= 0)', '        r = sd_bus_message_read_array(m, \'y\', &v, &n);',
                      '      if (r >= 0)', '        r = sd_bus_message_exit_container(m);',
                      f'      n = n < sizeof(p->{sn}) ? n : sizeof(p->{sn});',
                      f'      if (r >= 0 && (n != p->{sn}_len || (n > 0 && memcmp(p->{sn}, v, n) != 0))) {{',
                      f'        if (n > 0)', f'          memcpy(p->{sn}, v, n);', f'        p->{sn}_len = (uint8_t)n;',
                      f'        bits |= {upper}_PROP_{sn.upper()};', '      }']
            elif ctype(t).endswith('*'):
                c += ['      const char *v = "";', f'      r = sd_bus_message_read(m, "v", "{t}", &v);',
                      f'      if (r >= 0 && strncmp(p->{sn}, v, sizeof(p->{sn}) - 1) != 0) {{',
                      f'        snprintf(p->{sn}, sizeof(p->{sn}), "%s", v);', f'        bits |= {upper}_PROP_{sn.upper()};', '      }']
            else:
                c += [f'      {"int" if t == "b" else ctype(t)} v = 0;', f'      r = sd_bus_message_read(m, "v", "{t}", &v);',
                      f'      if (r >= 0 && p->{sn} != v) {{', f'        p->{sn} = v;', f'        bits |= {upper}_PROP_{sn.upper()};', '      }']
        c += ['    } else', '      r = sd_bus_message_skip(m, "v");', '    if (r >= 0)', '      r = sd_bus_message_exit_container(m);', '  }',
              '  if (r >= 0)', '    r = sd_bus_message_exit_container(m);', '  if (changed != NULL)', '    *changed = bits;',
              '  return r;', '}', '']
        c += ['static int properties_changed(sd_bus_message *m, void *userdata, sd_bus_error *error) {', '  (void)error;',
              '  const char *iface;', '  if (sd_bus_message_read(m, "s", &iface) >= 0)',
              f'    (({prefix}_changed *)userdata)(sd_bus_message_get_path(m), m);', '  return 0;', '}', '',
              f'int {prefix}_watch_properties(sd_bus *bus, {prefix}_changed *changed) {{',
              f'  return sd_bus_add_match(bus, NULL, "type=\'signal\',sender=\'" {upper}_SERVICE "\',"',
              '                          "interface=\'org.freedesktop.DBus.Properties\',member=\'PropertiesChanged\',"',
              f'                          "arg0=\'" {upper}_NAME "\'", properties_changed, (void *)changed);', '}', '']
        if any(w for *_, w, _ in props):
            c += ['static int set_replied(sd_bus_message *m, void *userdata, sd_bus_error *error) {', '  (void)error;',
                  f'  const {prefix}_set_then *then = userdata;', '  then->done(sd_bus_message_get_error(m), then->userdata);',
                  '  return 0;', '}', '']
            for name, sn, t, w, _ in props:
                if w:
                    c.append(f'int {prefix}_set_{sn}(sd_bus *bus{at}, {var(t, "value")}, const {prefix}_set_then *then) {{')
                    c.append(f'  return sd_bus_call_method_async(bus, NULL, {upper}_SERVICE, {at_arg}, "org.freedesktop.DBus.Properties", "Set",')
                    c.append(f'                                  then != NULL ? set_replied : NULL, (void *)then, "ssv", {upper}_NAME, "{name}", "{t}", '
                             f'{"(int)" if t == "b" else ""}value);')
                    c += ['}', '']

    for name, sn, args, _ in signals:
        basic = [(t, n) for t, n in args if t != 'a{sv}']
        c.append(f'static int on_{sn}(sd_bus_message *m, void *userdata, sd_bus_error *error) {{')
        c.append('  (void)error;')
        call = ', '.join(['sd_bus_message_get_path(m)'] + [n for _, n in basic] + (['m'] if len(basic) < len(args) else []))
        if basic:
            lines, addrs = read_vars(basic)
            c += lines
            c.append(f'  if (sd_bus_message_read(m, "{"".join(t for t, _ in basic)}", {addrs}) >= 0)')
            c.append(f'    (({prefix}_{sn}_fn *)userdata)({call});')
        else:
            c.append(f'  (({prefix}_{sn}_fn *)userdata)({call});')
        c += ['  return 0;', '}', '',
              f'int {prefix}_watch_{sn}(sd_bus *bus, {prefix}_{sn}_fn *fn) {{',
              f'  return sd_bus_add_match(bus, NULL, "type=\'signal\',sender=\'" {upper}_SERVICE "\',interface=\'" {upper}_NAME "\',"',
              f'                          "member=\'{name}\'", on_{sn}, (void *)fn);', '}', '']

    for suffix, lines in (('.h', h), ('-server.c', s), ('-client.c', c)):
        with open(f'{out}/{prefix}{suffix}', 'w') as f:
            f.write('\n'.join(lines).rstrip('\n') + '\n')


if __name__ == '__main__':
    main()
