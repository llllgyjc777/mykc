#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""crc_dump.py —— 从设备官方 .ko 导出「真 CRC 真值表」

为什么需要:
  GKI 内核 CONFIG_MODVERSIONS=y 时, insmod 会逐个比对外部符号的 CRC。
  外部模块自己没有 Module.symvers (modules_prepare 不生成), 所以
  直接编译出来的 .ko 里 __versions 是空的, 加载会被拒。
  v14 的解法是塞一个 size=0 的假段 —— 等于关闭 ABI 校验, 危险。

  正确解法: 设备 /vendor/lib/modules/*.ko 里的 __versions 段就是
  内核认可的 CRC 真值表。dump 出来直接用就行。

__versions 段格式 (本机实测):
  每项 64 字节 = u32 crc(小端, 低字节有效) + u32 pad + char name[56]

用法:
  python3 crc_dump.py /vendor/lib/modules > crcs.txt
  或
  python3 crc_dump.py file1.ko file2.ko ... > crcs.txt

输出格式 (add_versions2.py 可直接读):
  符号名 0xdeadbeef
"""

import struct
import sys
import os
import glob

VERS_ENTRY = 64
VERS_NAME_LEN = 56


def sections(data):
    """返回 [(name, type, flags, addr, offset, size, ...), ...]"""
    if data[:4] != b'\x7fELF':
        return None
    shoff = struct.unpack_from('<Q', data, 0x28)[0]
    ent = struct.unpack_from('<H', data, 0x3a)[0]
    num = struct.unpack_from('<H', data, 0x3c)[0]
    sx = struct.unpack_from('<H', data, 0x3e)[0]
    if ent == 0 or shoff + num * ent > len(data):
        return None
    FMT = '<IIQQQQIIQQ'
    sh = [list(struct.unpack_from(FMT, data, shoff + i * ent)) for i in range(num)]
    if sx >= num:
        return None
    sst = sh[sx]
    st = data[sst[4]:sst[4] + sst[5]]
    out = []
    for h in sh:
        nm = st[h[0]:].split(b'\0', 1)[0].decode('utf-8', 'replace')
        out.append((nm, h))
    return out


def extract(path, table):
    try:
        with open(path, 'rb') as f:
            data = f.read()
    except OSError:
        return
    secs = sections(data)
    if not secs:
        return
    for nm, h in secs:
        if nm != '__versions':
            continue
        off, size = h[4], h[5]
        if size == 0 or off + size > len(data):
            continue
        for p in range(off, off + size, VERS_ENTRY):
            if p + VERS_ENTRY > off + size:
                break
            crc, _pad = struct.unpack_from('<II', data, p)
            raw = data[p + 8:p + VERS_ENTRY]
            name = raw.split(b'\0', 1)[0].decode('utf-8', 'replace')
            if not name:
                continue
            if name not in table:
                table[name] = crc


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)

    args = sys.argv[1:]
    files = []
    for a in args:
        if os.path.isdir(a):
            files.extend(sorted(glob.glob(os.path.join(a, '*.ko'))))
        elif os.path.isfile(a):
            files.append(a)

    if not files:
        sys.exit('!! 没找到任何 .ko')

    table = {}
    for f in files:
        extract(f, table)

    sys.stderr.write('# files scanned: %d, symbols: %d\n' % (len(files), len(table)))
    if not table:
        sys.exit('!! 没提取到任何 __versions 记录 (设备可能没开 MODVERSIONS)')

    for name in sorted(table):
        sys.stdout.write('%s 0x%08x\n' % (name, table[name]))


if __name__ == '__main__':
    main()