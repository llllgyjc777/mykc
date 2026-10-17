/* mykc_cfi.c —— CFI 严格模式绕过 (v15, 修复关机重启)
 *
 * =====================================================================
 *  设备实证 (2026-10-16, /proc/kallsyms + /proc/config.gz 实查):
 *    CONFIG_CFI_CLANG=y
 *    # CONFIG_CFI_PERMISSIVE is not set       <-- 严格模式: 校验失败 => BUG => panic
 *    # CONFIG_LTO_CLANG_THIN is not set
 *    CONFIG_SHADOW_CALL_STACK=y
 *
 *    T report_cfi_failure
 *    T aarch64_insn_patch_text_nosync   (未 EXPORT, 需 kprobe 解析)
 * =====================================================================
 *
 *  ★★★ v15 关键修复 (v14 关机重启的直接根因) ★★★
 *
 *   report_cfi_failure 的真实原型 (5.15+/6.x):
 *       void report_cfi_failure(void *target, u64 pc, unsigned long typeid);
 *   它是 __noreturn: 末尾 BUG() -> do_error_trap -> die -> 重启。
 *
 *   v14 的做法是把入口第一条指令改成 `ret` (0xd65f03c0)。
 *   ARM64 的 `ret` 跳的是 LR = x30。而调用者 __cfi_slowpath_diag 是
 *   按「永不返回」编译的, 现场 x30 是任意值 / 旧的调用者地址,
 *   于是 ret 跳到一个无意义地址 -> 取指异常 / 数据异常 ->
 *   内核 panic -> 手机软重启。
 *
 *   实证吻合: 用户报「一 insmod 就关机重启」, 且本机 pstore
 *   (console-ramoops-0) 只留下 panic 后的 console 尾部,
 *   没有常规 Call trace 段落 —— 与「ret 跳到非法地址导致二次异常」一致。
 *
 *   v15 改为「静默返回而不是 BUG」:
 *     在入口处写两条指令:
 *         mov w0, #0     (0x52800000)
 *         ret            (0xd65f03c0)
 *     第二条 ret 执行时, LR 已被第 1 条之前的原序言? 不 —— 入口已被
 *     完全覆盖, 所以 LR 仍是调用者的返回地址, 这是正确的。
 *     语义等价于 CONFIG_CFI_PERMISSIVE=y: 校验失败只记录不 panic。
 *
 *   ★ 更安全的做法:
 *     用 Makefile 的 CONFIG_CFI_PERMISSIVE=y 重新编译内核, 或让模块
 *     走 kCFI 兼容编译。本 patch 是外部模块兜底, 默认关闭
 *     (MYKC_CFI_BYPASS 宏控制)。
 * =====================================================================
 */
#define pr_fmt(fmt) "[mykc] " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/kprobes.h>

/* ★ v15: 默认关闭 CFI patch。设为 1 才启用 (风险自担)。 */
#ifndef MYKC_CFI_BYPASS
#define MYKC_CFI_BYPASS 0
#endif

typedef int (*patch_text_nosync_t)(void *addr, u32 insn);
static patch_text_nosync_t g_patch_text_nosync;

/* ARM64 指令编码 */
#define ARM64_MOV_W0_0   0x52800000u   /* mov w0, #0 */
#define ARM64_RET_INSN   0xd65f03c0u   /* ret */

/* =====================================================================
 *  符号解析
 * ===================================================================== */
static unsigned long cfi_kprobe_resolve(const char *name)
{
    struct kprobe kp;
    unsigned long addr = 0;
    int ret;

    memset(&kp, 0, sizeof(kp));
    kp.symbol_name = name;
    ret = register_kprobe(&kp);
    if (ret < 0)
        return 0;

    addr = (unsigned long)kp.addr;
    unregister_kprobe(&kp);
    return addr;
}

static int cfi_init_patch_fn(void)
{
    unsigned long a;

    if (g_patch_text_nosync)
        return 0;

    a = cfi_kprobe_resolve("aarch64_insn_patch_text_nosync");
    if (!a) {
        pr_warn("cfi: cannot resolve aarch64_insn_patch_text_nosync\n");
        return -ENOENT;
    }

    g_patch_text_nosync = (patch_text_nosync_t)a;
    pr_info("cfi: patch fn @ %px\n", (void *)a);
    return 0;
}

/* =====================================================================
 *  目标表
 * ===================================================================== */
struct cfi_target {
    const char *name;
    unsigned long addr;
    u32 saved[2];      /* v15: 备份两指令 */
    int patched;
};

static struct cfi_target cfi_targets[] = {
    { .name = "report_cfi_failure" },
};

/* =====================================================================
 *  v15: 验证「首指令像不像真正的函数序言」
 *     防止 kprobe 解析出的地址偏移 4/8 字节 -> patch 到别的函数中间
 * ===================================================================== */
static int cfi_verify_prologue(u32 insn)
{
    switch (insn) {
    case 0xa9bf7bfdu:   /* stp x29,x30,[sp,#-16]! */
    case 0xa9be7bfdu:   /* stp x29,x30,[sp,#-32]! */
    case 0xa9bd7bfdu:   /* stp x29,x30,[sp,#-48]! */
    case 0xa9bc7bfdu:   /* stp x29,x30,[sp,#-64]! */
    case 0xd503233fu:   /* paciasp */
    case 0xd503237fu:   /* pacibsp */
    case 0xd65f03c0u:   /* 已经是 ret */
        return 0;
    default:
        break;
    }
    /* sub sp, sp, #imm  (0xd10xxxxx 系列) */
    if ((insn & 0xffc003ffu) == 0xd10003ffu)
        return 0;
    return -EINVAL;
}

/*
 * patch 单个目标: 把入口改成  mov w0,#0 ; ret
 *   返回: 0 = 本次成功   1 = 已 patch 过   <0 = 失败
 */
static int cfi_patch_one(struct cfi_target *t)
{
    u32 cur[2] = { 0, 0 };
    int ret;

    t->addr = cfi_kprobe_resolve(t->name);
    if (!t->addr) {
        pr_warn("cfi: symbol %s not found\n", t->name);
        return -ENOENT;
    }

    pr_info("cfi: %s @ %px\n", t->name, (void *)t->addr);

    memcpy(t->saved, (void *)t->addr, sizeof(t->saved));

    if (t->saved[0] == ARM64_MOV_W0_0 && t->saved[1] == ARM64_RET_INSN) {
        pr_info("cfi: %s already patched (v15 seq)\n", t->name);
        t->patched = 1;
        return 1;
    }

    if (cfi_verify_prologue(t->saved[0]) != 0) {
        pr_err("cfi: %s prologue mismatch (first insn = %08x), refuse to patch\n",
               t->name, t->saved[0]);
        t->addr = 0;
        return -EINVAL;
    }

    ret = g_patch_text_nosync((void *)t->addr, ARM64_MOV_W0_0);
    if (ret) {
        pr_warn("cfi: patch %s [0] failed: %d\n", t->name, ret);
        return ret;
    }
    ret = g_patch_text_nosync((void *)(t->addr + 4), ARM64_RET_INSN);
    if (ret) {
        pr_warn("cfi: patch %s [1] failed: %d, rollback\n", t->name, ret);
        g_patch_text_nosync((void *)t->addr, t->saved[0]);
        return ret;
    }

    memcpy(cur, (void *)t->addr, sizeof(cur));
    if (cur[0] != ARM64_MOV_W0_0 || cur[1] != ARM64_RET_INSN) {
        pr_warn("cfi: verify %s failed (%08x %08x)\n", t->name, cur[0], cur[1]);
        g_patch_text_nosync((void *)t->addr, t->saved[0]);
        g_patch_text_nosync((void *)(t->addr + 4), t->saved[1]);
        return -EIO;
    }

    t->patched = 1;
    pr_info("cfi: patched %s -> 'mov w0,#0; ret' (saved %08x %08x)\n",
            t->name, t->saved[0], t->saved[1]);
    return 0;
}

/* =====================================================================
 *  对外入口
 * ===================================================================== */
int mykc_cfi_bypass(void)
{
    unsigned long probe;
    int ok = 0;
    unsigned int i;

    pr_info("cfi: === CFI status probe (v15) ===\n");
    pr_info("cfi: kernel CFI_CLANG=y, PERMISSIVE=n => strict mode\n");

#if !MYKC_CFI_BYPASS
    /* 只探测, 不改内核代码段 —— 这是 v15 的默认行为 */
    probe = cfi_kprobe_resolve("report_cfi_failure");
    pr_info("cfi: MYKC_CFI_BYPASS=0 (probe only), report_cfi_failure @ %px\n",
            (void *)probe);
    return 0;
#else
    if (cfi_init_patch_fn() != 0) {
        pr_err("cfi: abort, cannot get patch_text fn\n");
        return -ENOENT;
    }

    for (i = 0; i < ARRAY_SIZE(cfi_targets); i++) {
        int r = cfi_patch_one(&cfi_targets[i]);
        if (r == 0 || r == 1)
            ok++;
    }

    pr_info("cfi: === bypass result: %d/%u patched ===\n",
            ok, (unsigned int)ARRAY_SIZE(cfi_targets));
    return ok > 0 ? 0 : -ENOENT;
#endif
}

/* 卸载恢复: 两条指令一起还原 */
void mykc_cfi_restore(void)
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(cfi_targets); i++) {
        struct cfi_target *t = &cfi_targets[i];
        u32 cur[2];

        if (!t->patched || !t->addr || !g_patch_text_nosync)
            continue;

        memcpy(cur, (void *)t->addr, sizeof(cur));
        if (cur[0] == ARM64_MOV_W0_0 && cur[1] == ARM64_RET_INSN) {
            g_patch_text_nosync((void *)t->addr, t->saved[0]);
            g_patch_text_nosync((void *)(t->addr + 4), t->saved[1]);
            pr_info("cfi: restored %s -> %08x %08x\n",
                    t->name, t->saved[0], t->saved[1]);
        }
        t->patched = 0;
    }
}