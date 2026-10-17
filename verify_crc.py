#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""verify_crc.py —— 校验 crcs.txt 是否与指定 .ko 一致 (自检工具)

用法:
  python3 verify_crc.py <某个官方.ko> <crcs.txt>
"""
import struct
import sys

FMT = '<IIQQQQIIQQ'
VERS_ENTRY = 64


def main():
    ko, table_path = sys.argv[1], sys.argv[2]

    table = {}
    with open(table_path, errors='replace') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            p = line.split()
            if len(p) >= 2:
                table[p[0]] = int(p[1], 16)

    d = open(ko, 'rb').read()
    shoff = struct.unpack_from('<Q', d, 0x28)[0]
    ent = struct.unpack_from('<H', d, 0x3a)[0]
    num = struct.unpack_from('<H', d, 0x3c)[0]
    sx = struct.unpack_from('<H', d, 0x3e)[0]
    sh = [list(struct.unpack_from(FMT, d, shoff + i * ent)) for i in range(num)]
    st = d[sh[sx][4]:sh[sx][4] + sh[sx][5]]

    ok = bad = miss = 0
    for h in sh:
        nm = st[h[0]:].split(b'\0', 1)[0].decode('utf-8', 'replace')
        if nm != '__versions':
            continue
        off, size = h[4], h[5]
        for p in range(off, off + size, VERS_ENTRY):
            if p + VERS_ENTRY > off + size:
                break
            crc = struct.unpack_from('<I', d, p)[0]
            name = d[p + 8:p + VERS_ENTRY].split(b'\0', 1)[0].decode('utf-8', 'replace')
            if not name:
                continue
            if name not in table:
                miss += 1
                continue
            if table[name] == crc:
                ok += 1
            else:
                bad += 1
                if bad <= 10:
                    print('MISMATCH %-40s ko=0x%08x table=0x%08x' %
                          (name, crc, table[name]))

    print('ko=%s' % ko)
    print('  match=%d  mismatch=%d  missing=%d' % (ok, bad, miss))
    sys.exit(0 if bad == 0 else 1)


if __name__ == '__main__':
    main()