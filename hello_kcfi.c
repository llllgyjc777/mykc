#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("kCFI probe module");

/* 间接调用点：通过函数指针调用 _printk, 必然生成 kCFI 检查 */
static int (*fn_ptr)(const char *fmt, ...) = _printk;

static int __init hk_init(void)
{
    fn_ptr("hello_kcfi: init via fn_ptr\n");
    return 0;
}

static void __exit hk_exit(void)
{
    _printk("hello_kcfi: exit\n");
}

module_init(hk_init);
module_exit(hk_exit);
