#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
fix_exit_off.py  -- 我的内核模块 struct module 布局修正器

背景:
  设备内核 (Redmi K80 Pro / SM8750 / 6.6.118-android15-8) 的 `struct module`
  含有  #if IS_ENABLED(CONFIG_KUNIT)  包起来的 16 字节字段:
      int                  num_kunit_suites;   // 偏移 1420, 4 + 4 填充
      struct kunit_suite **kunit_suites;       // 偏移 1424, 8
  因此:
      source_list  = 1432
      target_list  = 1448  (0x5a8)
      exit         = 1464  (0x5b8)
      refcnt       = 1472

  而我们编译所用的 GKI 源码树 olddefconfig 后 KUNIT 未真正启用,
  产出的 .ko 按 exit = 1448 (0x5a8) 生成重定位。
  内核按设备布局读 exit(1464) 时会读到 target_list/exit 之间错位后的垃圾,
  在 mod_sysfs_setup+0x2c0 处 ldr x0,[x8,#0x108] 触发 Oops。

  已验证的铁证:
      /vendor/lib/modules/binder_prio.ko    -> init 0x188, exit 0x5b8
      /vendor/lib/modules/ufs_ffu.ko        -> init 0x188, exit 0x5b8
      paradise p66.ko                       -> init 0x188, exit 0x5b8
      我们的产物                             -> init 0x188, exit 0x5a8  (错 16)

修正方法:
  找到 .rela.gnu.linkonce.this_module 段中 r_offset == 0x5a8 的那条重定位
  (它对应 cleanup_module), 把 r_offset 改成 0x5b8。
  只改 8 字节的 r_offset, 不动符号名与 addend。

用法:
  python3 fix_exit_off.py <file.ko> [more.ko ...]
  python3 fix_exit_off.py <dir>          # 递归修目录下所有 .ko

退出码:
  0  全部文件 exit r_offset 已是 0x5b8 (含本来就对的情况)
  2  一个文件都没被改 (可能没找到目标), 需要人工确认
  1  参数错误 / 文件无法解析
"""

import os
import struct
import sys

EXIT_OLD = 0x5A8          # 我们产物里错误的值 (1448)
EXIT_NEW = 0x5B8          # 设备真实布局 (1464)
INIT_OFF = 0x188          # init_module 重定位偏移, 用作布局指纹校验

RA64_ABS64 = 257


def find_sections(data):
    """解析 ELF64 section header, 返回 [(name, sh), ...]"""
    if data[:4] != b'\x7fELF':
        raise ValueError('not an ELF file')
    if data[4] != 2:
        raise ValueError('not ELF64')
    e_shoff = struct.unpack_from('<Q', data, 0x28)[0]
    e_shentsize = struct.unpack_from('<H', data, 0x3A)[0]
    e_shnum = struct.unpack_from('<H', data, 0x3C)[0]
    e_shstrndx = struct.unpack_from('<H', data, 0x3E)[0]

    shstr_off = struct.unpack_from('<Q', data, e_shoff + e_shstrndx * e_shentsize + 0x18)[0]

    out = []
    for i in range(e_shnum):
        base = e_shoff + i * e_shentsize
        sh_name = struct.unpack_from('<I', data, base)[0]
        sh_type = struct.unpack_from('<I', data, base + 0x04)[0]
        sh_offset = struct.unpack_from('<Q', data, base + 0x18)[0]
        sh_size = struct.unpack_from('<Q', data, base + 0x20)[0]
        sh_entsize = struct.unpack_from('<Q', data, base + 0x38)[0]

        end = data.find(b'\0', shstr_off + sh_name)
        name = data[shstr_off + sh_name:end].decode('utf-8', 'replace')

        out.append({
            'index': i,
            'name': name,
            'type': sh_type,
            'offset': sh_offset,
            'size': sh_size,
            'entsize': sh_entsize,
        })
    return out, e_shoff, e_shentsize


def fix_one(path, dry_run=False):
    """
    返回 (status, detail)
      status: 'fixed' | 'already' | 'notfound' | 'error'
    """
    try:
        with open(path, 'rb') as f:
            data = bytearray(f.read())
    except Exception as e:
        return 'error', 'read failed: %s' % e

    try:
        secs, shoff, shentsize = find_sections(data)
    except Exception as e:
        return 'error', 'parse failed: %s' % e

    rela = None
    for s in secs:
        if s['name'] == '.rela.gnu.linkonce.this_module':
            rela = s
            break

    if rela is None:
        return 'error', 'no .rela.gnu.linkonce.this_module section'

    entsize = rela['entsize'] or 24
    if entsize != 24:
        return 'error', 'unexpected rela entsize %d' % entsize

    n = rela['size'] // entsize
    base = rela['offset']

    found_init = False
    targets = []

    for i in range(n):
        p = base + i * entsize
        # Elf64_Rela: r_offset(8) r_info(8) r_addend(8)
        r_offset = struct.unpack_from('<Q', data, p)[0]
        r_info = struct.unpack_from('<Q', data, p + 8)[0]
        r_type = r_info & 0xFFFFFFFF

        if r_offset == INIT_OFF:
            found_init = True

        if r_type == RA64_ABS64 and r_offset in (EXIT_OLD, EXIT_NEW):
            targets.append((p, r_offset, i))

    if not found_init:
        return 'error', 'layout fingerprint mismatch: no init @ 0x188'

    if not targets:
        return 'notfound', 'no ABS64 reloc at 0x5a8/0x5b8'

    changed = 0
    for p, r_offset, i in targets:
        if r_offset == EXIT_OLD:
            if not dry_run:
                struct.pack_into('<Q', data, p, EXIT_NEW)
            changed += 1
            print('  [fix] entry[%d] r_offset 0x%x -> 0x%x' % (i, EXIT_OLD, EXIT_NEW))
        else:
            print('  [ok ] entry[%d] r_offset already 0x%x' % (i, r_offset))

    if changed and not dry_run:
        with open(path, 'wb') as f:
            f.write(data)

    # 复验
    if not dry_run:
        verify_ok = verify_one(path)
        if not verify_ok:
            return 'error', 'verify failed after write'

    return ('fixed' if changed else 'already'), '%d reloc entries, %d changed' % (len(targets), changed)


def verify_one(path):
    """复验: 读回文件, 确认 exit 重定位是 0x5b8"""
    try:
        with open(path, 'rb') as f:
            data = f.read()
        secs, _, _ = find_sections(data)
    except Exception:
        return False

    for s in secs:
        if s['name'] != '.rela.gnu.linkonce.this_module':
            continue
        entsize = s['entsize'] or 24
        n = s['size'] // entsize
        for i in range(n):
            p = s['offset'] + i * entsize
            r_offset = struct.unpack_from('<Q', data, p)[0]
            r_info = struct.unpack_from('<Q', data, p + 8)[0]
            if (r_info & 0xFFFFFFFF) == RA64_ABS64 and r_offset == EXIT_NEW:
                return True
        return False
    return False


def collect(paths):
    files = []
    for p in paths:
        if os.path.isdir(p):
            for root, _, names in os.walk(p):
                for nm in names:
                    if nm.endswith('.ko'):
                        files.append(os.path.join(root, nm))
        elif os.path.isfile(p):
            files.append(p)
        else:
            print('!! skip (not found): %s' % p)
    return sorted(set(files))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('-')]
    dry = any(a in ('-n', '--dry-run') for a in sys.argv[1:])

    if not args:
        print('usage: fix_exit_off.py <file.ko|dir> [more...] [-n]')
        return 1

    files = collect(args)
    if not files:
        print('!! no .ko files found')
        return 1

    print('=== exit r_offset fix: 0x%x -> 0x%x ===' % (EXIT_OLD, EXIT_NEW))

    n_fixed = 0
    n_bad = 0

    for f in files:
        print('[%s]' % f)
        st, detail = fix_one(f, dry_run=dry)
        if st == 'fixed':
            n_fixed += 1
            print('  => FIXED (%s)' % detail)
        elif st == 'already':
            print('  => already correct (%s)' % detail)
        elif st == 'notfound':
            print('  => WARN: %s' % detail)
        else:
            n_bad += 1
            print('  => ERROR: %s' % detail)

    print()
    print('=== SUMMARY ===')
    print('  files scanned : %d' % len(files))
    print('  fixed         : %d' % n_fixed)
    print('  already ok    : %d' % (len(files) - n_fixed - n_bad))
    print('  errors        : %d' % n_bad)

    if n_bad:
        print('RESULT: FAIL')
        return 1
    if n_fixed == 0:
        print('RESULT: OK (nothing needed changing)')
        return 0
    print('RESULT: OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
