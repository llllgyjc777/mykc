#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
strip_versions.py -- 从 .ko 中彻底移除 __versions 段

原理:
    Linux 内核 load_module() -> check_modinfo() 之后会调用
    check_version(info, "name", mod) 校验每个符号的 CRC:

        static int check_version(const struct load_info *info,
                                 const char *symname, struct module *mod) {
            unsigned int versionsize;
            const struct modversion_info *versions;

            if (!get_modinfo(info, "vermagic"))   /* 这里只管 vermagic */
                return 0;

            versions = (void *)info->sechdrs[info->index.versions].sh_addr;
            versionsize = info->sechdrs[info->index.versions].sh_size
                          / sizeof(*versions);

            if (versionsize == 0) {
                if (mod)
                    set_bit(TAINT_MODULE_INVALID, &mod->taints);
                return 1;   /* ★ 无段时直接放行 */
            }
            ...
        }

    所以: __versions 段不存在 (index.versions == 0) -> versionsize == 0 -> 放行。
    注意 v14 写 size=0 的空段会走 TAINT 分支, 这里改成真正删段, 语义不同。

做法:
    把 __versions 的 sh_type 改成 SHT_NULL 并把 sh_size 清零,
    同时清掉 sh_link / sh_info。ELF 加载器会把它当成空段跳过,
    info->index.versions 保持有效但内容长度为 0。

    更彻底的做法是同时把 .gnu.version 相关的记录也清掉 —— 但 .ko
    没有 .gnu.version (那是动态链接的), 所以这里只需处理 __versions。

用法:
    python3 strip_versions.py <file.ko>
"""

import struct
import sys
import os

SHT_NULL = 0
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

    ei_class = data[4]      # 1=32bit, 2=64bit
    ei_data = data[5]       # 1=LE, 2=BE

    if ei_class != 2:
        print("!! only ELF64 supported")
        return 1
    if ei_data != 1:
        print("!! only little-endian supported")
        return 1

    # ELF64 header
    e_shoff = struct.unpack_from("<Q", data, 0x28)[0]
    e_shentsize = struct.unpack_from("<H", data, 0x3A)[0]
    e_shnum = struct.unpack_from("<H", data, 0x3C)[0]
    e_shstrndx = struct.unpack_from("<H", data, 0x3E)[0]

    if e_shoff == 0 or e_shnum == 0:
        print("!! no section headers")
        return 1

    # 取 shstrtab
    shstr_off = e_shoff + e_shstrndx * e_shentsize
    shstrtab_fileoff = struct.unpack_from("<Q", data, shstr_off + 0x18)[0]
    shstrtab_size = struct.unpack_from("<Q", data, shstr_off + 0x20)[0]
    shstrtab = bytes(data[shstrtab_fileoff:shstrtab_fileoff + shstrtab_size])

    found = 0
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        sh_name_idx = struct.unpack_from("<I", data, off)[0]
        # 取名字
        end = shstrtab.find(b"\x00", sh_name_idx)
        name = shstrtab[sh_name_idx:end].decode("utf-8", "replace") if end > 0 else ""

        if name == "__versions":
            old_type = struct.unpack_from("<I", data, off + 4)[0]
            old_size = struct.unpack_from("<Q", data, off + 0x20)[0]
            old_link = struct.unpack_from("<I", data, off + 0x28)[0]
            old_info = struct.unpack_from("<I", data, off + 0x2C)[0]
            print("[found] section #%d __versions: type=%d size=%d link=%d info=%d"
                  % (i, old_type, old_size, old_link, old_info))

            # sh_type = SHT_NULL
            struct.pack_into("<I", data, off + 4, SHT_NULL)
            # sh_size = 0
            struct.pack_into("<Q", data, off + 0x20, 0)
            # sh_link = 0
            struct.pack_into("<I", data, off + 0x28, 0)
            # sh_info = 0
            struct.pack_into("<I", data, off + 0x2C, 0)
            # sh_addralign = 0
            struct.pack_into("<Q", data, off + 0x30, 0)
            # sh_entsize = 0
            struct.pack_into("<Q", data, off + 0x38, 0)
            found += 1

    if found == 0:
        print("[warn] __versions section not found, nothing to do")
        # 仍然写回, 保持幂等
    else:
        print("[ok] stripped %d __versions section(s)" % found)

    open(path, "wb").write(data)
    print("[done] wrote", path, len(data), "bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
