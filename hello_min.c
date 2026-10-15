// hello_min.c - minimal module for env validation
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>

static int __init hello_min_init(void)
{
    pr_info("hello_min: loaded ok\n");
    return 0;
}

static void __exit hello_min_exit(void)
{
    pr_info("hello_min: unloaded\n");
}

module_init(hello_min_init);
module_exit(hello_min_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mykc-test");
MODULE_DESCRIPTION("Minimal hello module");
MODULE_VERSION("0.1");
