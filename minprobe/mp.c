// mp.c — MYKC 最小装载验证模块
// 目的：判断 .ko 装载失败到底出在「编译链/工具链/CRC/vermagic」还是「mykc 具体代码」
//
// 设计原则（按 STEP 逐级放开，默认 STEP=0 最安全）：
//   STEP 0 : 只有 pr_info，init 立即返回 0。无任何分配、无函数指针调用。
//   STEP 1 : + vzalloc 16KB 然后 vfree
//   STEP 2 : + kzalloc 1KB 然后 kfree
//   STEP 3 : + alloc_page 然后 __free_page
//   STEP 4 : + misc_register / misc_deregister
//   STEP 5 : + register_kprobe(report_cfi_failure) —— 头号嫌疑项
//
// 判定方法：
//   STEP 0 能装载   -> 编译链 OK，问题在 mykc 代码
//   STEP 0 就崩     -> 编译链/CRC/工具链层面问题，代码怎么改都没用
//
// 编译：把本目录整体作为 M= 目标，Makefile 里 obj-m += mp.o

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/gfp.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/kprobes.h>

#ifndef MP_STEP
#define MP_STEP 0
#endif

#define MP_TAG "mp: "

// 编译期固定字符串，便于 dmesg 里能确认版本一致
static const char *mp_ver = "MP_BUILD_v1_STEP" MP_STR(MP_STEP);

static struct miscdevice mp_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "minprobe",
	.fops  = NULL,   /* 不提供操作接口，纯占位 */
};

#if MP_STEP >= 5
static int mp_kprobe_pre(struct kprobe *p, struct pt_regs *regs)
{
	return 0;
}
static struct kprobe mp_kp = {
	.symbol_name = "report_cfi_failure",
	.pre_handler = mp_kprobe_pre,
};
#endif

static int __init mp_init(void)
{
	int ret = 0;

	pr_info(MP_TAG "loaded, step=%d, ver=%s\n", MP_STEP, mp_ver);

#if MP_STEP >= 1
	{
		void *p = vzalloc(16 * 1024);
		if (!p) {
			pr_err(MP_TAG "vzalloc failed\n");
			return -ENOMEM;
		}
		pr_info(MP_TAG "step1 vzalloc ok @ %px\n", p);
		vfree(p);
	}
#endif

#if MP_STEP >= 2
	{
		void *p = kzalloc(1024, GFP_KERNEL);
		if (!p) {
			pr_err(MP_TAG "kzalloc failed\n");
			return -ENOMEM;
		}
		pr_info(MP_TAG "step2 kzalloc ok @ %px\n", p);
		kfree(p);
	}
#endif

#if MP_STEP >= 3
	{
		struct page *pg = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!pg) {
			pr_err(MP_TAG "alloc_page failed\n");
			return -ENOMEM;
		}
		pr_info(MP_TAG "step3 alloc_page ok\n");
		__free_page(pg);
	}
#endif

#if MP_STEP >= 4
	ret = misc_register(&mp_misc);
	if (ret) {
		pr_err(MP_TAG "step4 misc_register failed: %d\n", ret);
		return ret;
	}
	pr_info(MP_TAG "step4 misc_register ok (/dev/minprobe)\n");
	misc_deregister(&mp_misc);
	pr_info(MP_TAG "step4 misc_deregister ok\n");
#endif

#if MP_STEP >= 5
	ret = register_kprobe(&mp_kp);
	if (ret) {
		pr_err(MP_TAG "step5 register_kprobe failed: %d\n", ret);
		return ret;
	}
	pr_info(MP_TAG "step5 register_kprobe ok @ %px\n", mp_kp.addr);
	unregister_kprobe(&mp_kp);
	pr_info(MP_TAG "step5 unregister_kprobe ok\n");
#endif

	pr_info(MP_TAG "init done, returning 0\n");
	return 0;
}

static void __exit mp_exit(void)
{
	pr_info(MP_TAG "exit\n");
}

module_init(mp_init);
module_exit(mp_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mykc");
MODULE_DESCRIPTION("MyKernelCore minimal load probe");
MODULE_VERSION("1.0");
