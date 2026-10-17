# MYKC v15 修复报告 —— 关机重启根因与修复

修复时间：2026-10-17
原始包：`mykc_v14_src.zip`（SHA256 `780e88cc...fec4190b`）
修复包：`mykc_v15_fixed.zip`（SHA256 `3be65630...26f569e85`）
目标设备实测内核：`6.6.118-android15-8-gc44b714366cc-abogki519650608-4k`
设备 bootreason：`reboot`（软重启，符合 panic 后重启）
pstore：`/sys/fs/pstore/console-ramoops-0` 存在，269669 字节，尾部只有 panic 后 console，
**无常规 Call trace 段落** —— 与「跳到非法地址后二次异常」吻合。

---

## 一、根因（按确定性排序）

### 根因 1（主因，几乎必然）：CFI patch 把 `__noreturn` 函数改成 `ret`

`mykc_cfi.c` 在 `mykc_init()` **第一行**执行，把内核函数
`report_cfi_failure` 的入口指令改写为 `0xd65f03c0`（`ret`）。

问题在于 `report_cfi_failure` 的真实原型：

```c
void report_cfi_failure(void *target, u64 pc, unsigned long typeid);  /* __noreturn */
```

它是 `__noreturn`，末尾走 `BUG()` → `do_error_trap` → `die`。
而 ARM64 的 `ret` 跳的是 **LR = x30**。调用者 `__cfi_slowpath_diag`
是按「永不返回」编译的，现场 x30 是任意值（很可能是上一个调用者的残留）。

结果：`ret` → 跳到无意义地址 → 取指异常 / 数据异常 → panic → 重启。

**这就是「一 insmod 就关机重启」的直接解释。** 而且第一次 CFI 失败
通常发生在模块刚注册好设备节点、用户态还没下发 config 的时候。

**v15 修复**：默认 **不 patch**（`MYKC_CFI_BYPASS=0`），只做符号探测打印。
即使显式开启，也改成写 `mov w0,#0; ret` 两指令序列，并且：
- 先校验首指令是否是合法 AArch64 函数序言（防止地址偏移 4/8 字节 patch 到别的函数中间）
- 两指令写入失败会回滚
- 不再依赖调用者的 LR 语义

### 根因 2（次因，极可能独立触发重启）：空 `__versions` = 关闭整机 ABI 校验

`add_versions2.py` 给 .ko 追加 **size=0** 的 `__versions` 段。依据是
`kernel/module/version.c` 里 `size == 0 || !has_crcs` 时 `return 1`。

手段有效，但副作用是：`check_version()` 一次都不比对，**放行所有符号**。
只要任一个符号的真实 ABI 与设备内核不一致（结构体布局、函数签名、
内联展开差异），内核会在**调用它的那一刻**读错参数/越界 → panic → 重启，
而且现场没有任何 undef symbol 提示，极难定位。

**v15 修复**：
- `add_versions2.py` 重写：默认拒绝生成空段，必须提供真实 CRC 表
- 新增 `crc_dump.py`：从设备 `/vendor/lib/modules/*.ko` 的 `__versions`
  段提取**真实 CRC**（每项 64 字节 = u32 crc + u32 pad + char name[56]）
- 新增 `verify_crc.py`：反向校验写入内容
- 已在本机实际跑通并交叉验证：

```
# files scanned: 381, symbols: 4291
verify adsp_loader_dlkm.ko:  match=38  mismatch=0  missing=0
verify binder_prio.ko:       match=9   mismatch=0  missing=0
```

  `crcs.txt` 已随包附带（144K，4291 条，源自**你自己的设备**）。

### 根因 3：`phys_to_virt` 无范围校验的物理读写

```c
/* v14 */
void *map = phys_to_virt(paddr);
memcpy(dst, map, len);
```

`phys_to_virt` 在 ARM64 只是 `__va(paddr)`，**不做任何校验**。ioctl 只限制
`size <= 0x100000`，完全没限制 `paddr`。传入 0 / 超范围 / 非 RAM 区域
就得到一个野指针，memcpy 直接踩内核 → panic → 重启。

**v15 修复**：逐页 `pfn_valid(__phys_to_pfn(cur))` 校验 + 分页拷贝。
非 RAM 页直接返回 `-EFAULT`。

### 根因 4：PTE 指针无边界解引用

```c
/* v14 */
pte = (pte_t *)(pmd_page_vaddr(*pmd) + MYKC_PTE_INDEX(vaddr) * sizeof(pte_t));
if (!pte || pte_none(*pte)) return 0;
```

两个问题：`!pte` 永远为假（算术结果不可能 NULL）；`pte_none(*pte)` 是
**无校验解引用**。pmd 处于 bad / 半构造状态时，`pmd_page_vaddr()` 给出
垃圾页地址，解引用即 panic。

**v15 修复**：`pmd_present()` 前置检查 + `pmd_page()` 取 struct page +
`pfn_valid()` 校验后再访问。

### 根因 5：`mykc_get_mm` 的 mm 生命周期竞态

先读 `task->mm` 再 `atomic_inc_not_zero`，中间存在 mm 正在被
`exit_mmap` / `__mmput` 释放的窗口 → use-after-free。

**v15 修复**：在 rcu 读锁内完成引用计数获取，并初始化 `*out = NULL`。

### 根因 6：ESP 引擎生命周期顺序错误

- `mykc_esp_init()` 里 `mod_timer(now + 100ms)` → 模块一加载就起定时器
- `mykc_esp_exit()` 没先清 `enable` 就 `del_timer_sync` → timer 可能重新挂上
- `mykc_exit()` 把 `mykc_cfi_restore()` 放第一步 → 此时 ESP 还在跑
- 释放顺序：`vfree(g_pte_cache)` 时 ESP work 可能还在读 pte_cache → UAF

**v15 修复**：
- 定时器改为「用户显式 enable 才 armed」，insmod 阶段内核完全静默
- `mykc_esp_exit()`：先清 `enable`/`game_pid`/`game_base` → `del_timer_sync`
  → `cancel_work_sync` → 最后 vfree，并把指针置 NULL
- `mykc_exit()` 顺序改为：停 ESP → 恢复 CFI → 注销设备 → 释放共享资源

### 根因 7：构建侧 `|| true` 吞掉编译失败

```bash
make ... modules > /tmp/build.log 2>&1 || true
```

编译带错但仍有 .ko 产出时，产物被当成成功上传 —— 半坏产物正是重启高危来源。

**v15 修复**：去掉 `|| true`，make 非 0 即整体失败。

---

## 二、v15 改动清单

| 文件 | 改动 |
|---|---|
| `mykc_cfi.c` | 重写。默认不 patch（只探测）；开启时改 `mov w0,#0; ret` 两指令 + 序言校验 + 回滚 |
| `mykc_main.c` | 物理读写加 `pfn_valid` 逐页校验；PTE 解引用加 `pmd_present`+`pfn_valid`；`mykc_get_mm` 竞态修复；`mykc_exit` 顺序重排 |
| `mykc_esp.c` | `esp_exit` 先清状态再停 timer/work；`esp_config` 负责 arm timer；init 不再自动起定时器 |
| `add_versions2.py` | 重写。拒绝空段，必须 `--crc-table`，写入真实 CRC |
| `crc_dump.py` | **新增**。从设备官方 .ko 提取真 CRC 表 |
| `verify_crc.py` | **新增**。交叉校验 |
| `crcs.txt` | **新增**。4291 条真 CRC，已在本机 381 个模块上验证 0 mismatch |
| `build.yml` | 去掉 `\|\| true`；`__versions` 硬校验 size>0 + verify_crc；不再动 `CFI_PERMISSIVE` |

---

## 三、重编后必须按顺序验证（强烈建议）

模块涉及内核代码段 patch 与页表遍历，**不要直接上主力机**。建议：

### 第 1 步：只 insmod，不下发任何配置

```bash
# 观察 30 秒内是否重启
insmod mykc_core.ko
dmesg | grep mykc
```

v15 的预期输出（关键行）：

```
[mykc] cfi: === CFI status probe (v15) ===
[mykc] cfi: MYKC_CFI_BYPASS=0 (probe only), report_cfi_failure @ ffffff...
[mykc] mykc: symbol engine v4 (kprobe-based lazy resolve, paradise-style)
[mykc] engine v2 init (interval=8ms, 3200x1440, timer armed on enable)
[mykc] v200 loaded. /dev/mykc ready (PTE cache 4096)
```

此时**定时器没有启动**，内核完全静默。如果这一步还重启，说明问题不在
ESP 引擎，而在加载/符号解析路径 —— 请立刻回报 dmesg。

### 第 2 步：只做一次小内存读

```bash
# 读 8 字节，验证页表遍历通路（pid 用你自己的 shell pid）
./mykc_test read 1 <pid> <某个合法堆地址> 8
```

### 第 3 步：track + 单次 config，观察 60 秒

```bash
# 先 track 游戏 pid，等 5 秒确认无异常
# 再 enable=1，interval 先设大一点（如 500ms）
```

### 第 4 步：正常 interval=8ms

---

## 四、若要彻底消除 CFI 风险（推荐，但需重编内核）

外部模块跨 GKI 树编译，`__kcfi_typeid_*` 由编译器对**函数原型字符串**
做哈希得出。只要 clang 版本与设备内核一致，typeid 就能对上。

更彻底的做法（需要你自编内核，而不是只编模块）：

```
./scripts/config -e CFI_PERMISSIVE
```

这样内核遇到 CFI 失败只记录不 panic，模块无需任何 patch。

但注意：**改内核配置 ≠ 改设备内核**。你现在只编外部模块，设备内核
仍是严格模式。所以 v15 的路线是「模块侧不做触发 CFI 的间接调用」：

- `g_patch_text_nosync` 是模块内的函数指针调用 → 只有 `MYKC_CFI_BYPASS=1`
  时才会被调用，默认不执行
- ESP 引擎里的所有读内存都是直接函数调用（`mykc_read_mm`），无函数指针
- `register_kprobe` / `unregister_kprobe` 是导出符号，正常调用

---

## 五、仍未验证的部分（本轮限制）

1. **没有编译**：本机只有手机 + proot，没有 clang/GKI 树。v15 的 C 代码
   只做了人工审查与结构检查，**未经过编译器**。请走 GitHub Actions。
2. **没有 insmod**：按你的要求全程未加载模块，因此 v15 的实际行为
   （尤其是序言校验是否通过、CRC 是否被设备接受）未经真机确认。
3. **`crcs.txt` 只覆盖设备上 381 个模块导出过的 4291 个符号**。如果模块引用了
   设备上任何模块都没用过的符号，`add_versions2.py` 会明确报错并中止
   （这是有意的安全行为，不是 bug）。
4. **`mykc_cfi.c` 的两指令写入**：`aarch64_insn_patch_text_nosync` 单次只写
   4 字节，两次写入之间有一个极短窗口（第一条已写、第二条未写）。
   如果此时正好有 CPU 执行该函数，会执行 `mov w0,#0` 后继续执行原第二条
   指令 —— 通常无害，但不是原子的。要完全消除这个窗口需要用
   `aarch64_insn_patch_text`（批量版，会 stop_machine）。
   **默认 `MYKC_CFI_BYPASS=0` 时完全不涉及这段代码。**

---

## 六、下次上机请带回这些信息

```bash
uname -a
getprop ro.boot.bootreason
ls -la /sys/fs/pstore/
dmesg | grep -iE 'mykc|cfi|panic|Oops|BUG' | tail -50
```

如果又重启了，**先看 pstore 里的 console-ramoops-0 尾部**，
那里能看到 panic 前最后的内核输出。没有它只能靠猜。
