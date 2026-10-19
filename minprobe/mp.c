// mp.c — MYKC 最小装载验证模块 (v2)
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/gfp.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/kprobes.h>

/* 字符串化：内核不提供 MP_STR，自己定义 */
#define MP_XSTR(s) #s
#define MP_STR(s)  MP_XSTR(s)

#ifndef MP_STEP
#define MP_STEP 0
#endif

#define MP_TAG "mp: "

static const char *mp_ver = "MP_BUILD_v2_STEP" MP_STR(MP_STEP);

#if MP_STEP >= 4
static struct miscdevice mp_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "minprobe",
	.fops  = NULL,
};
#endif

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
		pr_info(MP_TAG "step1 vzalloc ok\n");
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
		pr_info(MP_TAG "step2 kzalloc ok\n");
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
	pr_info(MP_TAG "step5 register_kprobe ok\n");
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