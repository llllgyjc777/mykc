#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
patch_vermagic.py v2 -- 把内核模块 .ko 的 vermagic 改成目标值。
做法: 在 .modinfo 段的 vermagic 处变长 delta 后:
  1) head(0..mo) + new_seg + tail(mo+msz..EOF) 拼接 (EOF 之后所有内容整体后移 delta)
  2) 重定位 section header table: 它在文件里也整体后移了 delta, 用 e_shoff+delta 定位
  3) 段表内每个 sh_offset >= mo+msz 的 -> +delta
  4) 修正 .modinfo 的 sh_size, 修正 e_shoff
只改文件布局, 不改代码, 不改符号。
用法: python3 patch_vermagic.py <in.ko> <out.ko> "<目标版本-本地版>"
"""
import sys, struct

def main():
    if len(sys.argv) < 4:
        print("usage: patch_vermagic.py <in.ko> <out.ko> <target_ver>")
        sys.exit(1)
    src, dst, target_ver = sys.argv[1], sys.argv[2], sys.argv[3]
    d = bytearray(open(src, 'rb').read())

    assert d[:4] == b'\x7fELF', "not elf"
    assert d[4] == 2, "elf64 only"
    e_shoff = struct.unpack_from('<Q', d, 0x28)[0]
    e_shentsize = struct.unpack_from('<H', d, 0x3a)[0]
    e_shnum = struct.unpack_from('<H', d, 0x3c)[0]
    e_shstrndx = struct.unpack_from('<H', d, 0x3e)[0]
    print("[*] shoff=%d entsize=%d shnum=%d" % (e_shoff, e_shentsize, e_shnum))

    def read_sec(dd, base, i):
        b = base + i * e_shentsize
        no  = struct.unpack_from('<I', dd, b)[0]
        off = struct.unpack_from('<Q', dd, b + 0x18)[0]
        size= struct.unpack_from('<Q', dd, b + 0x20)[0]
        return no, off, size

    shstr_off = struct.unpack_from('<Q', d, e_shoff + e_shstrndx*e_shentsize + 0x18)[0]
    def secname(nameoff):
        end = d.index(b'\x00', shstr_off + nameoff)
        return bytes(d[shstr_off + nameoff:end]).decode('latin1')

    # 找 .modinfo
    mi_idx = None
    for i in range(e_shnum):
        no, off, size = read_sec(d, e_shoff, i)
        if no < 0xFFFFFFFF and secname(no) == '.modinfo':
            mi_idx, mi_off, mi_size = i, off, size
            break
    if mi_idx is None:
        print("[!] no .modinfo"); sys.exit(1)
    print("[*] .modinfo idx=%d off=%d size=%d" % (mi_idx, mi_off, mi_size))
    mo, msz = mi_off, mi_size

    seg = bytes(d[mo:mo+msz])
    k = seg.find(b'vermagic=')
    if k < 0:
        print("[!] no vermagic="); sys.exit(1)
    vstart = k + len(b'vermagic=')
    vend = seg.find(b'\x00', vstart)
    old_full = seg[vstart:vend]
    sp = old_full.find(b' ')
    suffix = old_full[sp+1:]
    new_full = target_ver.encode() + b' ' + suffix
    print("[*] old: %s" % old_full[:sp].decode())
    print("[*] new: %s" % target_ver)
    delta = len(new_full) - len(old_full)
    print("[*] delta=%d" % delta)

    if delta == 0:
        d[mo+vstart:mo+vstart+len(old_full)] = new_full
        open(dst, 'wb').write(d)
        print("[+] same-len -> %s" % dst); return

    # 拼接新文件
    new_seg = seg[:vstart] + new_full + seg[vstart+len(old_full):]
    assert len(new_seg) == msz + delta
    dnew = bytearray(d[:mo] + new_seg + d[mo+msz:])

    # section header table 在文件里也后移了 delta (只要 e_shoff >= mo+msz)
    shift_threshold = mo + msz
    new_shoff = e_shoff + delta if e_shoff >= shift_threshold else e_shoff
    print("[*] e_shoff %d -> %d (threshold %d)" % (e_shoff, new_shoff, shift_threshold))

    # 逐段修正 sh_offset
    fixed = 0
    for i in range(e_shnum):
        b = new_shoff + i * e_shentsize
        off = struct.unpack_from('<Q', dnew, b + 0x18)[0]
        if off >= shift_threshold:
            struct.pack_into('<Q', dnew, b + 0x18, off + delta)
            fixed += 1
    print("[*] fixed %d section offsets" % fixed)

    # 修正 .modinfo 的 size
    mi_b = new_shoff + mi_idx * e_shentsize
    struct.pack_into('<Q', dnew, mi_b + 0x20, msz + delta)
    # 修正 e_shoff
    struct.pack_into('<Q', dnew, 0x28, new_shoff)

    open(dst, 'wb').write(dnew)
    print("[+] done -> %s (%d -> %d)" % (dst, len(d), len(dnew)))

if __name__ == '__main__':
    main()