#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
strip_versions.py -- v31 (paradise 同款做法)

核心依据: 逆向 paradise p66.ko 得到的真实形态
    [45] __versions  PROGBITS  0000000000000000 0eb600 000000 00  A  0  0  8
    insmod p66.ko -> RC=0, 装载成功

旧脚本做法与它的差异:
    paradise : sh_type = PROGBITS(1), sh_size = 0, sh_addralign = 8
    旧脚本   : sh_type = NULL(0),     sh_size = 0, sh_addralign = 0
    ELF 规范里 SHT_NULL 表示"此节头表项未被使用", 有被特殊处理的风险。
    paradise 的 PROGBITS+size=0 是最保守、最安全的形态。

内核判定逻辑 (kernel/module/version.c check_version):
    versionsize = info->sechdrs[info->index.versions].sh_size / sizeof(*versions);
    if (versionsize == 0) { set_bit(TAINT_MODULE_INVALID, &mod->taints); return 1; }
    内核只看 sh_size。所以 size=0 就是放行, 不需要改 sh_type。

用法: python3 strip_versions.py <file.ko>
"""
import struct
import sys
import os

SHT_PROGBITS = 1


def main():
    if len(sys.argv) < 2:
        print("usage: strip_versions.py <file.ko>")
        return 1

    path = sys.argv[1]
    if not os.path.exists(path):
        print("!! file not found:", path)
        return 1

    data = bytearray(open(path, "rb").read())
    if data[:4] != b"\x7fELF":
        print("!! not an ELF file")
        return 1
    if data[4] != 2 or data[5] != 1:
        print("!! only ELF64 little-endian supported")
        return 1

    e_shoff = struct.unpack_from("<Q", data, 0x28)[0]
    e_shentsize = struct.unpack_from("<H", data, 0x3A)[0]
    e_shnum = struct.unpack_from("<H", data, 0x3C)[0]
    e_shstrndx = struct.unpack_from("<H", data, 0x3E)[0]

    if e_shoff == 0 or e_shnum == 0:
        print("!! no section headers")
        return 1

    shstr_off = e_shoff + e_shstrndx * e_shentsize
    shstr_fileoff = struct.unpack_from("<Q", data, shstr_off + 0x18)[0]
    shstr_size = struct.unpack_from("<Q", data, shstr_off + 0x20)[0]
    shstrtab = bytes(data[shstr_fileoff:shstr_fileoff + shstr_size])

    found = 0
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        name_off = struct.unpack_from("<I", data, off)[0]
        end = shstrtab.find(b"\x00", name_off)
        name = shstrtab[name_off:end].decode("utf-8", "replace") if end > 0 else ""

        if name != "__versions":
            continue

        old_type = struct.unpack_from("<I", data, off + 4)[0]
        old_size = struct.unpack_from("<Q", data, off + 0x20)[0]
        old_align = struct.unpack_from("<Q", data, off + 0x30)[0]
        print("[found] #%d __versions: type=%d size=%d align=%d"
              % (i, old_type, old_size, old_align))

        # paradise 形态
        struct.pack_into("<I", data, off + 4, SHT_PROGBITS)   # type = PROGBITS
        struct.pack_into("<Q", data, off + 0x20, 0)           # size = 0
        struct.pack_into("<Q", data, off + 0x30, 8)           # align = 8
        struct.pack_into("<Q", data, off + 0x38, 0)           # entsize = 0

        found += 1

    if found == 0:
        print("[warn] __versions section not found, nothing to do")

    open(path, "wb").write(data)
    print("[done] wrote", path, len(data), "bytes")
    print("[ok] format = PROGBITS + size=0 + align=8 (paradise 同款)")
    return 0


if __name__ == "__main__":
    sys.exit(main())