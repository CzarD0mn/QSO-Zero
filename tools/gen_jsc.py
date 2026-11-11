#!/usr/bin/env python3
"""Convert JS8Call's jsc_map.cpp / jsc_list.cpp into data/js8.jsc (binary, mmap-able).

Layout (little endian):
  char[8]  magic "NQJSC01\0"
  u32      nmap (262144), nlist, nprefix
  u32      map_off[nmap+1]   offsets into string blob
  u8       map_size[nmap]    the 'size' field (consumed chars; not always strlen!)
  u32      list_index[nlist] map index for each list entry (lookup order)
  struct { u8 ch; u8 pad; u16 pad; u32 size; u32 index; } prefix[nprefix]
  u8       blob[]            latin-1 strings
"""
import re, struct, sys
pat = re.compile(r'\{"((?:[^"\\]|\\.)*)"\s*(?:/\*.*?\*/)?\s*,\s*(\d+),\s*(\d+)\}')
def parse(txt):
    return [(m.group(1).encode('latin-1').decode('unicode_escape').encode('latin-1'),
             int(m.group(2)), int(m.group(3))) for m in pat.finditer(txt)]
src = sys.argv[1] if len(sys.argv) > 1 else '.'
mp = parse(open(src + '/jsc_map.cpp', encoding='latin-1').read())
lt = open(src + '/jsc_list.cpp', encoding='latin-1').read()
pi = lt.index('JSC::prefix')
ls, px = parse(lt[:pi]), parse(lt[pi:])
assert len(mp) == 262144 and len(ls) == 262144 and len(px) == 103, (len(mp), len(ls), len(px))
assert all(e[2] == i for i, e in enumerate(mp))
assert all(mp[e[2]][0] == e[0] for e in ls)
blob = b''.join(e[0] for e in mp)
offs, o = [], 0
for e in mp:
    offs.append(o); o += len(e[0])
offs.append(o)
out = bytearray(b'NQJSC01\0')
out += struct.pack('<3I', len(mp), len(ls), len(px))
out += struct.pack('<%dI' % len(offs), *offs)
out += bytes(e[1] for e in mp)
out += struct.pack('<%dI' % len(ls), *[e[2] for e in ls])
for s, size, idx in px:
    assert len(s) == 1
    out += struct.pack('<BBHII', s[0], 0, 0, size, idx)
out += blob
open(sys.argv[2] if len(sys.argv) > 2 else 'js8.jsc', 'wb').write(out)
print('map', len(mp), 'list', len(ls), 'prefix', len(px), 'bytes', len(out),
      'size!=strlen:', sum(1 for e in mp if len(e[0]) != e[1]))
