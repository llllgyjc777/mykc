# -*- coding: utf-8 -*-
"""add_versions2.py —— v15: 不再伪造空 __versions, 改为写真实 CRC

================= 为什么 v14 的做法危险 =================
v14 给 .ko 追加一个 **size=0** 的 __versions 段。依据是
kernel/module/version.c:

    static int same_magic(const char *amagic, const char *bmagic, bool has_crcs)
    {
        if (has_crcs) { amagic += strcspn(amagic, " "); bmagic += ...; }
        return strcmp(amagic, bmagic) == 0;
    }
    ...
    has_crcs = info->index.vers > 0;      /* 存在 __versions 段即为 true */
    if (size == 0 || !has_crcs) return 1; /* size==0 -> 直接放行 */

结论没错: size=0 的 __versions 会让 check_version() 一次都不比对,
放行**所有**符号。等于把 CONFIG_MODVERSIONS 的 ABI 保护整个关掉。

后果: 只要任何一个符号的真实 ABI 与设备内核不一致(struct 布局、函数
签名、内联展开), 内核会在**调用该符号的那一刻**读到错位的数据 /
错误的参数 -> 越界 / 空指针 / 野指针 -> panic -> 重启。
而且现场完全看不出原因(没有任何 undef symbol 警告)。

================= v15 的做法 =================
用设备官方模块的 __versions 表当作「真 CRC 真值表」:
  /vendor/lib/modules/*.ko 里的 __versions 段每一项 64 字节:
      u32 crc (小端, 低字节有效) | u32 pad | char name[56]
  这不依赖缺失的 Module.symvers, 而是设备自己认可的 CRC。

用法:
  # 1) 先导出真值表 (在手机上跑, root)
  python3 crc_dump.py /vendor/lib/modules > crcs.txt
  # 2) 写进 ko
  python3 add_versions2.py mykc_core.ko --crc-table crcs.txt

  # 兜底模式 (无 crc 表时):
  python3 add_versions2.py mykc_core.ko --no-crcs
  —— 会显式打印警告: 这是关闭 ABI 校验, 崩溃风险自负。

校验模式:
  python3 add_versions2.py mykc_core.ko --check
"""

import struct
import sys
import os

FMT = '<IIQQQQIIQQ'
SEC_ENT = struct.calcsize(FMT)
VERS_ENTRY = 64          # 设备实证: 每项 64 字节
VERS_NAME_LEN = 56


def read_elf(path):
    with open(path, 'rb') as f:
        d = bytearray(f.read())
    if d[:4] != b'\x7fELF':
        sys.exit('!! not ELF: ' + path)
    shoff = struct.unpack_from('<Q', d, 0x28)[0]
    ent = struct.unpack_from('<H', d, 0x3a)[0]
    num = struct.unpack_from('<H', d, 0x3c)[0]
    sx = struct.unpack_from('<H', d, 0x3e)[0]
    if ent != SEC_ENT:
        sys.exit('!! unexpected shentsize %d' % ent)
    if shoff + num * ent > len(d):
        sys.exit('!! section table out of range')
    sh = [list(struct.unpack_from(FMT, d, shoff + i * ent)) for i in range(num)]
    sst = sh[sx]
    st = bytes(d[sst[4]:sst[4] + sst[5]])
    names = [st[h[0]:].split(b'\0', 1)[0].decode('utf-8', 'replace') for h in sh]
    return d, shoff, num, sx, sh, names, st


def parse_crc_table(path):
    """解析 crc 真值表: 每行 'sym_name 0xdeadbeef'"""
    table = {}
    with open(path, 'r', errors='replace') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            parts = line.split()
            if len(parts) < 2:
                continue
            name, crc = parts[0], parts[1]
            try:
                table[name] = int(crc, 16)
            except ValueError:
                continue
    return table


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)

    ko = sys.argv[1]
    check_only = '--check' in sys.argv
    no_crcs = '--no-crcs' in sys.argv
    crc_table_path = None
    if '--crc-table' in sys.argv:
        crc_table_path = sys.argv[sys.argv.index('--crc-table') + 1]

    d, shoff, num, sx, sh, names, st = read_elf(ko)
    print('file      = %s (%d bytes)' % (ko, len(d)))
    print('shnum     = %d  e_shoff=%d  shstrndx=%d' % (num, shoff, sx))
    print('has __versions = %s' % ('__versions' in names))

    if '__versions' in names:
        idx = names.index('__versions')
        vsz = sh[idx][5]
        print('__versions already present, size=%d' % vsz)
        if vsz > 0:
            if check_only:
                return
            print('OK: 已有非空 __versions, 无需改写')
            return
        # ★ 关键分支: 已存在但 size==0 (v14 的空段) —— 必须重新写入真实数据
        print('WARNING: 该 __versions 是空段(size=0), 等于关闭 ABI 校验!')
        print('         将原地重新写入真实 CRC (复用同一节头)。')
        reuse_vers_idx = idx
    else:
        reuse_vers_idx = None

    if check_only:
        print('--check only, not modified')
        return

    # ---- 收集需要版本记录的符号: UND (外部引用) 全部 ----
    sh_und = None
    # 找 .symtab / .dynsym 与对应 strtab
    und_syms = []
    for i, h in enumerate(sh):
        if h[1] != 2:   # SHT_SYMTAB
            continue
        sym_off, sym_size, sym_link, sym_entsize = h[4], h[5], h[6], h[9]
        if sym_entsize != 24:
            continue
        strtab = sh[sym_link]
        sdata = bytes(d[strtab[4]:strtab[4] + strtab[5]])
        for off in range(sym_off, sym_off + sym_size, 24):
            st_name, st_info, st_other, st_shndx = struct.unpack_from('<IBBH', d, off)
            nm = sdata[st_name:].split(b'\0', 1)[0].decode('utf-8', 'replace')
            if st_shndx == 0 and nm:   # SHN_UNDEF
                und_syms.append(nm)

    und_syms = sorted(set(und_syms))
    print('UND symbols to version: %d' % len(und_syms))

    # 内核自身符号不需要 CRC (module_layout 之外的都是导出符号, 需要)
    crc_map = {}
    if crc_table_path:
        crc_map = parse_crc_table(crc_table_path)
        print('crc table entries: %d' % len(crc_map))
    elif not no_crcs:
        sys.exit(
            '!! 拒绝执行: 未提供 --crc-table。\n'
            '   v14 的空 __versions 会关闭整机 ABI 校验, 是重启高风险源。\n'
            '   请先: python3 crc_dump.py /vendor/lib/modules > crcs.txt\n'
            '   再:   python3 add_versions2.py %s --crc-table crcs.txt\n'
            '   确实要走不安全路线再加 --no-crcs。' % ko)

    # ---- 构造 __versions 数据 ----
    payload = bytearray()
    missing = []
    for nm in und_syms:
        crc = crc_map.get(nm)
        if crc is None:
            if no_crcs:
                crc = 0
            else:
                missing.append(nm)
                continue
        nb = nm.encode('utf-8')[:VERS_NAME_LEN - 1]
        rec = struct.pack('<II', crc & 0xFFFFFFFF, 0) + nb.ljust(VERS_NAME_LEN, b'\0')
        assert len(rec) == VERS_ENTRY
        payload += rec

    if missing:
        sys.exit('!! 以下符号在 crc 表中找不到, 为保证安全中止:\n   ' +
                 '\n   '.join(missing[:40]))

    if no_crcs:
        print('=' * 60)
        print('!!!!! WARNING: --no-crcs 模式 !!!!!')
        print('  crc 全部写成 0, 等于关闭 ABI 校验 (只保留段存在性)。')
        print('  一旦某个符号 ABI 不匹配, 运行时就是 panic / 重启。')
        print('=' * 60)

    # v14 会追加一个 size=0 的空段; 这里写真实数据。
    data_off = len(d)
    d += payload

    if reuse_vers_idx is not None:
        # 复用已有空的 __versions 节头: 只改数据指针与大小
        sh[reuse_vers_idx][4] = data_off
        sh[reuse_vers_idx][5] = len(payload)
        added = 0
    else:
        # 新 shstrtab 追加在数据之后
        new_st = st + b'__versions\0'
        name_off = len(st)
        st_off = len(d)
        d += new_st

        sh[sx][4] = st_off
        sh[sx][5] = len(new_st)

        sh.append([name_off, 1, 2, 0, data_off, len(payload), 0, 0, 8, 0])
        added = 1

    new_shoff = len(d)
    for h in sh:
        d += struct.pack(FMT, *h)

    struct.pack_into('<Q', d, 0x28, new_shoff)
    struct.pack_into('<H', d, 0x3c, num + added)

    with open(ko, 'wb') as f:
        f.write(d)

    print('PATCHED (real CRCs):')
    print('  entries = %d  payload = %d bytes' % (len(payload) // VERS_ENTRY, len(payload)))
    print('  shnum   %d -> %d' % (num, num + added))
    print('  size    %d -> %d' % (shoff, len(d)))


if __name__ == '__main__':
    main()