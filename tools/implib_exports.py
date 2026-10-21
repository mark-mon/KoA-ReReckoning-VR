#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Lists the exports described by a Windows import library (short import objects): "name ordinal-or-hint type".
import sys, struct
data = open(sys.argv[1], 'rb').read()
out = {}
pos = 8
while pos + 60 <= len(data):
    size = int(data[pos+48:pos+58].strip())
    body = data[pos+60:pos+60+size]
    if len(body) >= 20 and body[0:4] == b'\x00\x00\xff\xff':
        _, _, ver, mach, ts, sz, ordhint, typ = struct.unpack('<HHHHIIHH', body[:20])
        name, dll = body[20:].split(b'\0')[:2]
        name = name.decode(); ntype = (typ >> 2) & 7
        if ntype == 3:                  # undecorate: strip prefix and @N
            name = name.lstrip('_?@').split('@')[0]
        elif ntype == 2:
            name = name.lstrip('_?@')
        out[name] = (ordhint, ntype)
    pos += 60 + size + (size & 1)
for n, (o, t) in sorted(out.items()):
    print(n, o, t)
