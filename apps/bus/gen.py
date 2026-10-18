#!/usr/bin/env python3
"""
A D-Bus interface as C over conn.h (our own connection, no sd-bus), from its introspection XML:

    gen.py INTERFACE.xml OUTDIR

writes NAME.h, NAME-server.c and NAME-client.c, NAME the interface's last element in lower case
(org.senkos.System1: system1). The XML is one <node name="/path"> with one <interface>: the
service owns the interface's name on the bus and serves it at the node's path. Another service's
(org.bluez.Device1): <annotation name="org.senkos.Service" value="org.bluez"/> in the interface;
a node without a name, and each call takes the object's path. A Service of "" (a standard
interface anyone serves: org.freedesktop.DBus.Properties): each call takes the service too.
<annotation name="org.senkos.Prefix" value="wpa_iface"/> names it otherwise. A comment right
before a method, property or signal is its documentation, copied into NAME.h.

Signatures are parsed here, not at run time: each argument becomes its run of wire.h reads or
writes. Basic types are C values; s, o and g a str (common/str.h) pointing into the message, valid while
the callback runs (NUL-terminated there, so .p is a C string too).
Anything else is read as a bus_rd at its value, to read with wire.h (an a{sv}: bus_rd_kv), and
written by a bus_put the caller passes with its context.

Server (NAME-server.c): NAME_serve(bus, [path,] userdata) serves it. A call runs the service's

    int NAME_METHOD(bus_req *req, void *userdata, IN...);

which answers now or later (copying *req), with NAME_METHOD_reply(req, OUT...), bus_reply or
bus_reply_error; a negative errno answers with that error.

Client (NAME-client.c), nothing allocated per call:

    int NAME_call_METHOD(bus_conn *bus, [str service,] [str path,] IN..., bus_call *call, NAME_METHOD_done *done);

`call` (conn.h; NULL: no answer wanted) is the caller's, in what it's about: done(call, error, OUT...)
runs once from bus_process, the error NULL or the call's (OUT... zero then).

Properties (<property>, basic types and ay; strings a STRBUF(64), byte arrays cut at 64 bytes, an
ay's length in PROP_len): NAME_props, NAME_props_apply for an a{sv} (GetManagedObjects,
InterfacesAdded, PropertiesChanged, GetAll: the keys it has, a NAME_PROP_* bit each that changed),
NAME_set_PROP(bus, [path,] value, call, done) for the writable ones, and NAME_watch_properties for
the PropertiesChanged signals.

Signals: NAME_watch_SIGNAL(bus, fn), fn(object, ARGS...). One fn per signal per program: watching
again replaces it. Watches go with the connection: again on each new one.
"""
import re
import sys
import xml.etree.ElementTree as ET

STRING = 64
# D-Bus type: C type, wire.h's reader and writer
BASIC = {'y': ('uint8_t', 'u8'), 'b': ('bool', 'bool'), 'n': ('int16_t', 'i16'), 'q': ('uint16_t', 'u16'),
         'i': ('int32_t', 'i32'), 'u': ('uint32_t', 'u32'), 'x': ('int64_t', 'i64'), 't': ('uint64_t', 'u64'),
         'd': ('double', 'f64'), 's': ('str', 'str'), 'o': ('str', 'str'), 'g': ('str', 'sig')}
ALIGN = {'y': 1, 'g': 1, 'v': 1, 'n': 2, 'q': 2, 'b': 4, 'i': 4, 'u': 4, 's': 4, 'o': 4, 'a': 4,
         'x': 8, 't': 8, 'd': 8, '(': 8, '{': 8}
PROPERTIES = 'org.freedesktop.DBus.Properties'
# What generated functions call their own, beside arguments: a method's in, its out, a signal's
RESERVED_IN = {'bus', 'service', 'path', 'call', 'done', 'wr', 'req', 'userdata', 'msg', 'rd'}
RESERVED_OUT = {'call', 'msg', 'buf', 'err', 'rd', 'req', 'wr'}
RESERVED_SIGNAL = {'object', 'match', 'msg', 'rd'}


def fail(why):
    sys.exit(f'gen.py: {why}')


def snake(name):
    return re.sub(r'(?<=[a-z0-9])(?=[A-Z])|(?<=[A-Z])(?=[A-Z][a-z])', '_', name).lower()


def parse(sig, at=0):
    """One complete type of `sig` from `at`: (code, children), past it"""
    if at >= len(sig):
        fail(f'signature {sig!r} ends early')
    code = sig[at]
    at += 1
    if code in BASIC or code == 'v':
        return (code, []), at
    if code == 'a':
        child, at = parse(sig, at)
        return ('a', [child]), at
    if code in '({':
        close = ')' if code == '(' else '}'
        children = []
        while at < len(sig) and sig[at] != close:
            child, at = parse(sig, at)
            children.append(child)
        if at >= len(sig) or not children or (code == '{' and (len(children) != 2 or children[0][0] not in BASIC)):
            fail(f'invalid signature {sig!r}')
        return (code, children), at + 1
    fail(f'type {code!r} in {sig!r}: not so far')


def single(sig):
    node, at = parse(sig)
    if at != len(sig):
        fail(f'{sig!r}: one complete type an argument')
    return node


def skip(node, rd):
    """C statements past one value of `node`, its type known now"""
    code, children = node
    if code in 'sog':
        return [f'bus_rd_{BASIC[code][1]}({rd});']
    if code in BASIC:
        return [f'bus_rd_take({rd}, {ALIGN[code]}, {ALIGN[code]});']
    if code == 'v':
        return [f'bus_skip({rd}, bus_rd_sig({rd}));']
    if code == 'a':
        return [f'bus_rd_skip_to({rd}, bus_rd_array({rd}, {ALIGN[children[0][0]]}));']
    return [f'bus_rd_align({rd}, 8);'] + [s for c in children for s in skip(c, rd)]


def rtype(t):
    """What a reader gets for `t`: a C value, or a cursor at it"""
    return BASIC[t][0] if t in BASIC else 'bus_rd *'


def wparam(t, n):
    if t in BASIC:
        c = BASIC[t][0]
        return f'{c}{"" if c.endswith("*") else " "}{n}'
    return f'bus_put *{n}, const void *{n}_ctx'


def rparam(t, n):
    c = rtype(t)
    return f'{c}{"" if c.endswith("*") else " "}{n}'


def check(args, reserved, what):
    for t, n in args:
        single(t)
        if n in reserved:
            fail(f'{what}: an argument named {n}, generated code\'s own')


def reads(args, rd):
    """Locals for `args` read off `rd` (cursors left at complex ones), and what to pass on"""
    lines, passed = [], []
    for i, (t, n) in enumerate(args):
        if t in BASIC:
            c, fn = BASIC[t]
            lines.append(f'  {c}{"" if c.endswith("*") else " "}{n} = bus_rd_{fn}(&{rd});')
            passed.append(n)
        else:
            lines.append(f'  bus_rd {n} = {rd};')
            if i < len(args) - 1:
                lines += ['  ' + s for s in skip(single(t), '&' + rd)]
            passed.append(f'&{n}')
    return lines, passed


def writes(args, wr):
    out = []
    for t, n in args:
        if t in BASIC:
            out.append(f'  bus_wr_{BASIC[t][1]}({wr}, {n});')
        else:
            out.append(f'  {n}({wr}, {n}_ctx);')
    return out


def sig_of(args):
    return ''.join(t for t, _ in args)


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
        elif el.tag == 'method':
            args = {'in': [], 'out': []}
            for a in el.findall('arg'):
                args[a.get('direction', 'in')].append((a.get('type'), a.get('name') or f'arg{len(args["in"]) + len(args["out"])}'))
            check(args['in'], RESERVED_IN, el.get('name'))
            check(args['out'], RESERVED_OUT, el.get('name'))
            methods.append((el.get('name'), snake(el.get('name')), args['in'], args['out'], doc))
        elif el.tag == 'property':
            t = el.get('type')
            if t not in BASIC and t != 'ay':
                fail(f'{el.get("name")}: properties of basic types and ay alone so far')
            props.append((el.get('name'), snake(el.get('name')), t, 'write' in el.get('access', ''), doc))
        elif el.tag == 'signal':
            args = [(a.get('type'), a.get('name') or f'arg{i}') for i, a in enumerate(el.findall('arg'))]
            check(args, RESERVED_SIGNAL, el.get('name'))
            signals.append((el.get('name'), snake(el.get('name')), args, doc))
        else:
            fail(f'<{el.tag}>: methods, properties and signals alone so far')
        doc = None
    upper = prefix.upper()

    out = sys.argv[2]
    src = sys.argv[1].rsplit('/', 1)[-1]
    head = f'/* Generated by apps/bus/gen.py from {src}: not to be edited */'
    svc_param = ', str service' if service == '' else ''
    svc_arg = 'service' if service == '' else f'S({upper}_SERVICE)'
    path_param = ', str path' if path is None else ''
    path_arg = 'path' if path is None else f'S({upper}_PATH)'
    at = svc_param + path_param
    sender = '' if service == '' else f'sender=\'" {upper}_SERVICE "\','

    # The header
    h = [head, '#pragma once', '', '#include "conn.h"', '', f'#define {upper}_NAME "{iface}"']
    if service != '':
        h.append(f'#define {upper}_SERVICE "{service}"')
    if path is not None:
        h.append(f'#define {upper}_PATH "{path}"')
    h += ['', '/* The service\'s: served at the path (kept) for each call to run */',
          f'void {prefix}_serve(bus_conn *bus{path_param}, void *userdata);']
    for name, sn, ins, outs, d in methods:
        h.append('')
        if d:
            h.append(f'/* {d} */')
        h.append(f'int {prefix}_{sn}(bus_req *req, void *userdata{"".join(", " + rparam(t, n) for t, n in ins)});')
        if outs:
            h.append(f'int {prefix}_{sn}_reply(bus_req *req{"".join(", " + wparam(t, n) for t, n in outs)});')
    h += ['', '/* The client\'s */']
    for name, sn, ins, outs, _ in methods:
        h.append(f'typedef void {prefix}_{sn}_done(bus_call *call, const bus_error *error{"".join(", " + rparam(t, n) for t, n in outs)});')
        h.append(f'int {prefix}_call_{sn}(bus_conn *bus{at}{"".join(", " + wparam(t, n) for t, n in ins)}, bus_call *call, {prefix}_{sn}_done *done);')
    if props:
        h += ['', '/* Its properties as last told */', 'typedef struct {']
        for name, sn, t, _, d in props:
            field = (f'uint8_t {sn}[{STRING}]; uint8_t {sn}_len' if t == 'ay'
                     else f'STRBUF({STRING}) {sn}' if t in 'sog' else f'{BASIC[t][0]} {sn}')
            h.append(f'  {field};{f" /* {d} */" if d else ""}')
        h += [f'}} {prefix}_props;', 'enum {']
        h += [f'  {upper}_PROP_{sn.upper()} = 1u << {i},' for i, (_, sn, _, _, _) in enumerate(props)]
        h += ['};',
              f'/* An a{{sv}}, read through: the keys it has; in *changed (or NULL), a {upper}_PROP_* bit each',
              ' * that changed */',
              f'int {prefix}_props_apply({prefix}_props *p, bus_rd *rd, uint32_t *changed);',
              '/* PropertiesChanged: the object, its a{sv} to apply */',
              f'typedef void {prefix}_changed(str path, bus_rd *props);',
              f'int {prefix}_watch_properties(bus_conn *bus, {prefix}_changed *changed);']
        if any(w for *_, w, _ in props):
            h.append(f'typedef void {prefix}_set_done(bus_call *call, const bus_error *error);')
            for name, sn, t, w, _ in props:
                if w and t == 'ay':
                    fail(f'{name}: writable byte arrays not so far')
                if w:
                    h.append(f'int {prefix}_set_{sn}(bus_conn *bus{at}, {wparam(t, "value")}, bus_call *call, {prefix}_set_done *done);')
    for name, sn, args, d in signals:
        h.append('')
        if d:
            h.append(f'/* {d} */')
        h.append(f'typedef void {prefix}_{sn}_fn(str object{"".join(", " + rparam(t, n) for t, n in args)});')
        h.append(f'int {prefix}_watch_{sn}(bus_conn *bus, {prefix}_{sn}_fn *fn);')

    # The service's
    s = [head, '#include <string.h>', '', f'#include "{prefix}.h"', '',
         'static int dispatch(bus_req *req, const bus_msg *msg, void *userdata) {', '  bus_rd rd = msg->body;',
         '  (void)rd, (void)userdata;', '  switch (msg->member.n) {']
    for length in sorted({len(m[0]) for m in methods}):
        s.append(f'  case {length}:')
        for name, sn, ins, outs, _ in methods:
            if len(name) != length:
                continue
            lines, passed = reads(ins, 'rd')
            s += [f'    if (memcmp(msg->member.p, "{name}", {length}) == 0) {{',
                  f'      if (!str_eq(msg->sig, S("{sig_of(ins)}")))',
                  f'        return bus_reply_error(req, S(BUS_ERROR_INVALID_ARGS), S("{name} takes ({sig_of(ins)})"));']
            s += ['    ' + l for l in lines]
            s += ['      if (rd.bad)', f'        return bus_reply_error(req, S(BUS_ERROR_INVALID_ARGS), S("{name}: malformed"));',
                  f'      return {prefix}_{sn}(req, userdata{"".join(", " + p for p in passed)});', '    }']
        s.append('    break;')
    s.append('  }')
    s += ['  return bus_reply_error(req, S("org.freedesktop.DBus.Error.UnknownMethod"), msg->member);', '}', '',
          f'static bus_object object = {{.iface = S({upper}_NAME), .dispatch = dispatch}};', '',
          f'void {prefix}_serve(bus_conn *bus{path_param}, void *userdata) {{',
          f'  object.path = {path_arg};', '  object.userdata = userdata;', '  bus_add_object(bus, &object);', '}']
    for name, sn, ins, outs, _ in methods:
        if outs:
            s += ['', f'int {prefix}_{sn}_reply(bus_req *req{"".join(", " + wparam(t, n) for t, n in outs)}) {{',
                  f'  bus_wr *wr = bus_reply_begin(req, "{sig_of(outs)}");', *writes(outs, 'wr'),
                  '  return bus_reply_end(req);', '}']

    # The client's
    c = [head, '#include <errno.h>', '#include <string.h>', '', f'#include "{prefix}.h"']
    for name, sn, ins, outs, _ in methods:
        lines, passed = reads(outs, 'rd')
        zero = [f'    {n} = {"(bus_rd){.bad = true}" if t not in BASIC else "S(\"\")" if t in "sog" else "0"};' for t, n in outs]
        c += ['', f'static void {sn}_replied(bus_call *call, const bus_msg *msg) {{', '  bus_error buf;',
              '  const bus_error *err = bus_msg_error(msg, &buf);', '  bus_rd rd = msg->body;',
              f'  if (err == NULL && !str_eq(msg->sig, S("{sig_of(outs)}")))', '    err = &bus_malformed;']
        if outs:
            c += ['  if (err != NULL)', '    rd.bad = true;', *lines, '  if (err == NULL && rd.bad)', '    err = &bus_malformed;',
                  '  if (err != NULL) {', *zero, '  }']
        else:
            c.append('  (void)rd;')
        c += [f'  (({prefix}_{sn}_done *)call->done)(call, err{"".join(", " + p for p in passed)});', '}', '',
              f'int {prefix}_call_{sn}(bus_conn *bus{at}{"".join(", " + wparam(t, n) for t, n in ins)}, bus_call *call, {prefix}_{sn}_done *done) {{',
              f'  bus_wr *wr = bus_call_begin(bus, {svc_arg}, {path_arg}, S({upper}_NAME), S("{name}"), "{sig_of(ins)}",',
              '                              call != NULL);',
              *writes(ins, 'wr')]
        if not ins:
            c.append('  (void)wr;')
        c += [f'  return bus_call_end(bus, call, {sn}_replied, (void (*)(void))done);', '}']

    if props:
        c += ['', f'int {prefix}_props_apply({prefix}_props *p, bus_rd *rd, uint32_t *changed) {{', '  uint32_t bits = 0;',
              '  str key, sig;', '  const uint8_t *end = bus_rd_array(rd, 8);',
              '  while (bus_rd_kv(rd, end, &key, &sig)) {', '    switch (key.n) {']
        for length in sorted({len(name) for name, *_ in props}):
            c.append(f'    case {length}:')
            for name, sn, t, _, _ in props:
                if len(name) != length:
                    continue
                bit = f'{upper}_PROP_{sn.upper()}'
                c.append(f'      if (memcmp(key.p, "{name}", {length}) == 0 && str_eq(sig, S("{t}"))) {{')
                if t == 'ay':
                    c += ['        size_t vn;', '        const uint8_t *v = bus_rd_bytes(rd, &vn);',
                          f'        vn = vn < sizeof(p->{sn}) ? vn : sizeof(p->{sn});',
                          f'        if (vn != p->{sn}_len || (vn > 0 && memcmp(p->{sn}, v, vn) != 0)) {{',
                          f'          memcpy(p->{sn}, v, vn);', f'          p->{sn}_len = (uint8_t)vn;', f'          bits |= {bit};', '        }']
                elif t in 'sog':
                    c += [f'        const str v = bus_rd_{BASIC[t][1]}(rd);',
                          f'        if (!str_eq(sb_str(p->{sn}), str_head(v, sizeof(p->{sn}.p) - 1))) {{',
                          f'          sb_set(p->{sn}, v);', f'          bits |= {bit};', '        }']
                else:
                    c += [f'        const {BASIC[t][0]} v = bus_rd_{BASIC[t][1]}(rd);',
                          f'        if (p->{sn} != v) {{', f'          p->{sn} = v;', f'          bits |= {bit};', '        }']
                c += ['        continue;', '      }']
            c.append('      break;')
        c += ['    }', '    bus_skip(rd, sig);', '  }', '  if (changed != NULL)', '    *changed = bits;',
              '  return rd->bad ? -EBADMSG : 0;', '}', '',
              'static void properties_changed(bus_match *match, const bus_msg *msg) {', '  bus_rd rd = msg->body;',
              '  const str iface = bus_rd_str(&rd);',
              f'  if (str_eq(msg->sig, S("sa{{sv}}as")) && str_eq(iface, S({upper}_NAME)) && !rd.bad)',
              f'    (({prefix}_changed *)match->user)(msg->path, &rd);', '}', '',
              f'static bus_match properties_match = {{.iface = S("{PROPERTIES}"), .member = S("PropertiesChanged"),',
              '                                       .fn = properties_changed};', '',
              f'int {prefix}_watch_properties(bus_conn *bus, {prefix}_changed *changed) {{',
              '  properties_match.user = (void (*)(void))changed;',
              f'  return bus_add_match(bus, &properties_match, S("type=\'signal\',{sender}interface=\'{PROPERTIES}\',"',
              f'                       "member=\'PropertiesChanged\',arg0=\'" {upper}_NAME "\'"));', '}']
        if any(w for *_, w, _ in props):
            c += ['', 'static void set_replied(bus_call *call, const bus_msg *msg) {', '  bus_error buf;',
                  f'  (({prefix}_set_done *)call->done)(call, bus_msg_error(msg, &buf));', '}']
            for name, sn, t, w, _ in props:
                if w:
                    c += ['', f'int {prefix}_set_{sn}(bus_conn *bus{at}, {wparam(t, "value")}, bus_call *call, {prefix}_set_done *done) {{',
                          f'  bus_wr *wr = bus_call_begin(bus, {svc_arg}, {path_arg}, S("{PROPERTIES}"), S("Set"), "ssv", call != NULL);',
                          f'  bus_wr_str(wr, S({upper}_NAME));', f'  bus_wr_str(wr, S("{name}"));', f'  bus_wr_sig(wr, "{t}");',
                          *writes([(t, 'value')], 'wr'), '  return bus_call_end(bus, call, set_replied, (void (*)(void))done);', '}']

    for name, sn, args, _ in signals:
        lines, passed = reads(args, 'rd')
        c += ['', f'static void {sn}_signalled(bus_match *match, const bus_msg *msg) {{', '  bus_rd rd = msg->body;',
              f'  if (!str_eq(msg->sig, S("{sig_of(args)}")))', '    return;', *lines, '  if (!rd.bad)',
              f'    (({prefix}_{sn}_fn *)match->user)(msg->path{"".join(", " + p for p in passed)});', '}', '',
              f'static bus_match {sn}_match = {{.iface = S({upper}_NAME), .member = S("{name}"), .fn = {sn}_signalled}};', '',
              f'int {prefix}_watch_{sn}(bus_conn *bus, {prefix}_{sn}_fn *fn) {{',
              f'  {sn}_match.user = (void (*)(void))fn;',
              f'  return bus_add_match(bus, &{sn}_match, S("type=\'signal\',{sender}interface=\'" {upper}_NAME "\',member=\'{name}\'"));',
              '}']

    for suffix, lines in (('.h', h), ('-server.c', s), ('-client.c', c)):
        with open(f'{out}/{prefix}{suffix}', 'w') as f:
            f.write('\n'.join(lines).rstrip('\n') + '\n')


if __name__ == '__main__':
    main()
