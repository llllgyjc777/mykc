/* mykc_probe.c —— 最小可加载探针 (v16 step0)
 *
 * 目的: 定位「insmod 即重启」的根因。
 *
 * v15 的 init 路径依次做了:
 *   1) mykc_cfi_bypass()  -> register_kprobe("report_cfi_failure")
 *   2) vzalloc(PTE cache, 4096 * 32B = 128KB)
 *   3) kzalloc(hide_pid_map)
 *   4) alloc_page(ring)
 *   5) misc_register(/dev/mykc)
 *   6) mykc_esp_init()    -> 3 * vzalloc + timer_setup
 *
 * 这个最小版本只做:
 *   - 打印内核版本 (证明模块能进内核)
 *   - 分配一块小内存 (证明 kvalloc 通路)
 *   - misc_register 一个 /dev/mykc_probe (证明设备注册通路)
 *
 * 完全不做:
 *   - 不 register_kprobe   (最大嫌疑!)
 *   - 不 patch 内核代码
 *   - 不起 timer / workqueue
 *   - 不遍历页表
 *   - 不摘模块链表
 *
 * 如果这个版本能正常 insmod 而不重启, 说明基础框架 OK。
 * 然后用 MYKC_STEP 宏逐个放开功能, 定位到具体是哪一个炸的。
 */
#define pr_fmt(fmt) "[mykc-probe] " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/utsname.h>

#define MYKC_PROBE_DEV "mykc_probe"

/*
 * 分步开关: 编译时用 -DMYKC_STEP=n 控制
 *   0 = 只打印 (默认, 最安全)
 *   1 = + vzalloc 128KB
 *   2 = + kzalloc
 *   3 = + alloc_page
 *   4 = + misc_register
 *   5 = + register_kprobe("report_cfi_failure")   <- 疑似炸点
 */
#ifndef MYKC_STEP
#define MYKC_STEP 0
#endif

static void *g_test_valloc;
static void *g_test_kalloc;
static struct page *g_test_page;
static int g_misc_ok;

static long probe_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    pr_info("probe ioctl: cmd=0x%x arg=0x%lx\n", cmd, arg);
    return 0;
}

static const struct file_operations probe_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = probe_ioctl,
};

static struct miscdevice probe_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = MYKC_PROBE_DEV,
    .fops  = &probe_fops,
    .mode  = 0666,
};

static int __init probe_init(void)
{
    int ret;

    pr_info("=== step %d begin ===\n", MYKC_STEP);
    pr_info("kernel: %s\n", init_uts_ns.name.release);

#if MYKC_STEP >= 1
    pr_info("step1: vzalloc 128KB ...\n");
    g_test_valloc = vzalloc(128 * 1024);
    if (!g_test_valloc) {
        pr_err("step1 FAILED\n");
        return -ENOMEM;
    }
    pr_info("step1 OK: %px\n", g_test_valloc);
#endif

#if MYKC_STEP >= 2
    pr_info("step2: kzalloc 1KB ...\n");
    g_test_kalloc = kzalloc(1024, GFP_KERNEL);
    if (!g_test_kalloc) {
        pr_err("step2 FAILED\n");
        return -ENOMEM;
    }
    pr_info("step2 OK: %px\n", g_test_kalloc);
#endif

#if MYKC_STEP >= 3
    pr_info("step3: alloc_page ...\n");
    g_test_page = alloc_page(GFP_KERNEL | __GFP_ZERO);
    if (!g_test_page) {
        pr_err("step3 FAILED\n");
        return -ENOMEM;
    }
    pr_info("step3 OK\n");
#endif

#if MYKC_STEP >= 4
    pr_info("step4: misc_register /dev/%s ...\n", MYKC_PROBE_DEV);
    ret = misc_register(&probe_dev);
    if (ret) {
        pr_err("step4 FAILED: %d\n", ret);
        return ret;
    }
    g_misc_ok = 1;
    pr_info("step4 OK\n");
#endif

#if MYKC_STEP >= 5
    {
        struct kprobe kp;
        unsigned long addr = 0;

        pr_info("step5: register_kprobe(report_cfi_failure) ...\n");
        memset(&kp, 0, sizeof(kp));
        kp.symbol_name = "report_cfi_failure";
        ret = register_kprobe(&kp);
        if (ret < 0) {
            pr_warn("step5: register_kprobe failed: %d (symbol missing?)\n", ret);
        } else {
            addr = (unsigned long)kp.addr;
            pr_info("step5 OK: report_cfi_failure @ %px\n", (void *)addr);
            unregister_kprobe(&kp);
            pr_info("step5: kprobe removed\n");
        }
    }
#endif

    pr_info("=== step %d done, module loaded OK ===\n", MYKC_STEP);
    return 0;
}

static void __exit probe_exit(void)
{
    pr_info("unloading ...\n");

#if MYKC_STEP >= 4
    if (g_misc_ok) {
        misc_deregister(&probe_dev);
        g_misc_ok = 0;
    }
#endif
#if MYKC_STEP >= 3
    if (g_test_page) {
        __free_page(g_test_page);
        g_test_page = NULL;
    }
#endif
#if MYKC_STEP >= 2
    kfree(g_test_kalloc);
    g_test_kalloc = NULL;
#endif
#if MYKC_STEP >= 1
    vfree(g_test_valloc);
    g_test_valloc = NULL;
#endif

    pr_info("unloaded\n");
}

module_init(probe_init);
module_exit(probe_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("mykc");
MODULE_DESCRIPTION("MYKC minimal load probe");
MODULE_VERSION("16.0");