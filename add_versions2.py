# -*- coding: utf-8 -*-
# 给 .ko 追加「空的 __versions 段」(size=0) —— 设备内核 CONFIG_MODVERSIONS=y 必备
#
# 原理 (kernel/module/version.c 实证):
#   same_magic(a,b,has_crcs):  has_crcs=true 时跳过第一段(内核版本号)再比
#   has_crcs = info->index.vers  -> find_sec("__versions") 的段号
#   => 带 __versions 段(且 SHF_ALLOC)才能忽略版本号差异; 且 size=0 时
#      check_version 循环不执行, 直接 return 1 放行所有符号(仅一条 warn_once)
#
# 与 v1 的区别: v1 把 shstrtab 数据搬到 head+mid 之后, 隐含要求 shstrtab 是
#   最后一个有数据的段, 否则其它段 sh_offset 会左移 ssize 字节 -> 静默破坏.
#   v2 不移动任何已有段: 新 shstrtab 直接追加到文件末尾, 新节头表写在其后.
#   ELF 允许 e_shoff 指任意位置, 故完全合法; 旧的原地废弃(无害).
#
# 用法: python3 add_versions2.py <ko文件> [--check]

import struct
import sys

FPATH = sys.argv[1] if len(sys.argv) > 1 else 'mykc_core.ko'
CHECK_ONLY = '--check' in sys.argv

with open(FPATH, 'rb') as f:
    d = bytearray(f.read())

if d[:4] != b'\x7fELF':
    sys.exit('!! not ELF: ' + FPATH)

shoff = struct.unpack_from('<Q', d, 0x28)[0]
ent = struct.unpack_from('<H', d, 0x3a)[0]
num = struct.unpack_from('<H', d, 0x3c)[0]
sx = struct.unpack_from('<H', d, 0x3e)[0]

if shoff + num * ent > len(d):
    sys.exit('!! section table out of range')

FMT = '<IIQQQQIIQQ'
sh = [list(struct.unpack_from(FMT, d, shoff + i * ent)) for i in range(num)]
sst = sh[sx]
st = bytes(d[sst[4]:sst[4] + sst[5]])
names = [st[h[0]:].split(b'\0', 1)[0] for h in sh]

print('file      = %s (%d bytes)' % (FPATH, len(d)))
print('shnum     = %d  e_shoff=%d  shstrndx=%d' % (num, shoff, sx))
print('shstrtab  = off=%d size=%d' % (sst[4], sst[5]))
print('has __versions = %s' % (b'__versions' in names))

if b'__versions' in names:
    print('OK: __versions already present, nothing to do')
    sys.exit(0)

if CHECK_ONLY:
    print('--check only, not modified')
    sys.exit(0)

# ---- 追加新 shstrtab 数据 (不动任何已有字节) ----
new_st = st + b'__versions\0'
name_off = len(st)              # "__versions" 在新 strtab 中的偏移
st_off = len(d)                 # 新 shstrtab 数据偏移
d += new_st

# 更新 shstrtab 段头 (off/size)
sh[sx][4] = st_off
sh[sx][5] = len(new_st)

# 新增 __versions 段头: PROGBITS(1) + SHF_ALLOC(2), addr=0 off=0 size=0 align=8
sh.append([name_off, 1, 2, 0, 0, 0, 0, 0, 8, 0])

# ---- 新节头表写到文件末尾 ----
new_shoff = len(d)
for h in sh:
    d += struct.pack(FMT, *h)

struct.pack_into('<Q', d, 0x28, new_shoff)   # e_shoff
struct.pack_into('<H', d, 0x3c, num + 1)     # e_shnum

with open(FPATH, 'wb') as f:
    f.write(d)

print('PATCHED:')
print('  shnum  %d -> %d' % (num, num + 1))
print('  e_shoff %d -> %d' % (shoff, new_shoff))
print('  size   %d -> %d' % (shoff, len(d)))
print('  new __versions: name_off=%d flags=SHF_ALLOC size=0 align=8' % name_off)
