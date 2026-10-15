/* mykc_core.c —— MyKernelCore 内核模块主体（原创）
 *
 * 目标内核: 6.6.118-android15-8-GKI (REDMI K80 Pro / SM8750)
 *
 * 组成:
 *   [A] 内存引擎   页表遍历 + PTE 哈希缓存
 *   [B] 批量引擎   一次 ioctl 读 64 个散地址
 *   [C] 共享环     mmap 零拷贝
 *   [D] 隐藏引擎   modules 链表摘除
 *   [E] 符号引擎   kallsyms_lookup_name
 *   [F] fd 注入
 *   [G] ioctl 总入口（含 v2: track/config/frame）
 *
 * 与 mykc_esp.c 的分工:
 *   core 提供「能力」: mykc_read_mm / mykc_get_mm / mykc_put_mm
 *   esp  提供「引擎」: 自动追踪 + 内核态 W2S + 方框输出
 */
#define pr_fmt(fmt) "[mykc] " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/file.h>          /* fput / fd_install / get_unused_fd_flags */
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/pid.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/anon_inodes.h>
#include <linux/fdtable.h>
#include <linux/version.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/mmap_lock.h>
/* ★ 不 include <linux/kallsyms.h> —— 它会在符号表里引入 kallsyms_lookup_name/_text 的 UND 引用,
 *   导致 insmod 报 "Unknown symbol" (内核 5.7+ 这俩符号已不 EXPORT)。
 *   本模块改用 kprobe 动态获取地址 (见 kprobe_lookup_name), 无需该头文件。 */
#include <linux/kprobes.h>      /* ★ kprobe: 动态获取未导出符号 */
#include <linux/namei.h>
#include <linux/dcache.h>
#include <linux/list.h>
#include <linux/seq_file.h>
#include <linux/mm_types.h>
#include <linux/ktime.h>
#include <linux/mman.h>          /* vm_flags_set */
#include <asm/pgtable.h>
#include <asm/memory.h>
#include <asm/page.h>

#include "mykc.h"

/* =====================================================================
 *  全局统计
 * ===================================================================== */
static struct mykc_info g_info = {
    .version = MYKC_VERSION,
    .flags   = MYKC_F_CACHE,
};

/* =====================================================================
 *  [A] 内存引擎 —— PTE 哈希缓存 + 页表遍历
 * ===================================================================== */
#define PTE_CACHE_BITS   12
#define PTE_CACHE_SIZE   (1 << PTE_CACHE_BITS)
#define PTE_CACHE_MASK   (PTE_CACHE_SIZE - 1)

struct pte_cache_entry {
    u64 mm;
    u64 vaddr;
    u64 paddr;
    u64 stamp;
};

static struct pte_cache_entry *g_pte_cache;
static DEFINE_SPINLOCK(g_pte_lock);

static inline u32 pte_hash(u64 mm, u64 vaddr)
{
    u64 k = (mm >> 4) ^ (vaddr >> 12);
    k *= 0x9E3779B97F4A7C15ULL;
    return (u32)(k >> (64 - PTE_CACHE_BITS));
}

static int pte_cache_lookup(u64 mm, u64 vaddr, u64 *paddr)
{
    u32 h = pte_hash(mm, vaddr);
    struct pte_cache_entry *e;
    int hit = 0;

    spin_lock(&g_pte_lock);
    e = &g_pte_cache[h];
    if (e->mm == mm && e->vaddr == vaddr && e->paddr) {
        *paddr = e->paddr;
        e->stamp = jiffies;
        g_info.pte_cache_hits++;
        hit = 1;
    }
    spin_unlock(&g_pte_lock);
    return hit;
}

static void pte_cache_store(u64 mm, u64 vaddr, u64 paddr)
{
    u32 h = pte_hash(mm, vaddr);
    struct pte_cache_entry *e;

    spin_lock(&g_pte_lock);
    e = &g_pte_cache[h];
    e->mm = mm;
    e->vaddr = vaddr;
    e->paddr = paddr;
    e->stamp = jiffies;
    g_info.pte_cache_miss++;
    spin_unlock(&g_pte_lock);
}

static void pte_cache_flush(void)
{
    spin_lock(&g_pte_lock);
    memset(g_pte_cache, 0, sizeof(struct pte_cache_entry) * PTE_CACHE_SIZE);
    spin_unlock(&g_pte_lock);
}

/* 虚拟地址 → 物理地址（走目标进程页表，ARM64）*/
static u64 virt_to_phys_in_mm(struct mm_struct *mm, u64 vaddr)
{
    pgd_t *pgd;
    p4d_t *p4d;
    pud_t *pud;
    pmd_t *pmd;
    pte_t *pte;
    u64 paddr = 0;

    pgd = pgd_offset(mm, vaddr);
    if (pgd_none(*pgd) || pgd_bad(*pgd)) return 0;

    p4d = p4d_offset(pgd, vaddr);
    if (p4d_none(*p4d) || p4d_bad(*p4d)) return 0;

    pud = pud_offset(p4d, vaddr);
    if (pud_none(*pud) || pud_bad(*pud)) return 0;
    if (pud_leaf(*pud))
        return (pud_val(*pud) & PUD_MASK) | (vaddr & ~PUD_MASK);

    pmd = pmd_offset(pud, vaddr);
    if (pmd_none(*pmd) || pmd_bad(*pmd)) return 0;
    if (pmd_leaf(*pmd))
        return (pmd_val(*pmd) & PMD_MASK) | (vaddr & ~PMD_MASK);

    pte = pte_offset_map(pmd, vaddr);
    if (!pte || pte_none(*pte)) return 0;

    paddr = (pte_val(*pte) & PAGE_MASK) | (vaddr & ~PAGE_MASK);
    pte_unmap(pte);
    return paddr;
}

/* ---- 虚拟内存读（导出，供 esp 复用）---- */
int mykc_read_mm(struct mm_struct *mm, u64 vaddr, void *dst, size_t len)
{
    u64 paddr;
    void *map;
    size_t done = 0;

    while (done < len) {
        u64 cur = vaddr + done;
        u64 page_off = cur & ~PAGE_MASK;
        size_t chunk = min(len - done, PAGE_SIZE - page_off);

        if (!pte_cache_lookup((u64)mm, cur & PAGE_MASK, &paddr)) {
            paddr = virt_to_phys_in_mm(mm, cur);
            if (!paddr) return -EFAULT;
            pte_cache_store((u64)mm, cur & PAGE_MASK, paddr);
        }

        map = phys_to_virt(paddr & PAGE_MASK);
        if (!map) return -EFAULT;
        memcpy((char *)dst + done, (char *)map + (paddr & ~PAGE_MASK), chunk);
        done += chunk;
    }
    g_info.reads++;
    return 0;
}

/* ---- 虚拟内存写 ---- */
static int write_virt(struct mm_struct *mm, u64 vaddr, const void *src, size_t len)
{
    u64 paddr;
    void *map;
    size_t done = 0;

    while (done < len) {
        u64 cur = vaddr + done;
        u64 page_off = cur & ~PAGE_MASK;
        size_t chunk = min(len - done, PAGE_SIZE - page_off);

        if (!pte_cache_lookup((u64)mm, cur & PAGE_MASK, &paddr)) {
            paddr = virt_to_phys_in_mm(mm, cur);
            if (!paddr) return -EFAULT;
            pte_cache_store((u64)mm, cur & PAGE_MASK, paddr);
        }

        map = phys_to_virt(paddr & PAGE_MASK);
        if (!map) return -EFAULT;
        memcpy((char *)map + (paddr & ~PAGE_MASK), (const char *)src + done, chunk);
        done += chunk;
    }
    g_info.writes++;
    return 0;
}

/* ---- 物理地址读写 ---- */
static int read_phys_addr(u64 paddr, void *dst, size_t len)
{
    void *map = phys_to_virt(paddr);
    if (!map) return -EFAULT;
    memcpy(dst, map, len);
    g_info.reads++;
    return 0;
}

static int write_phys_addr(u64 paddr, const void *src, size_t len)
{
    void *map = phys_to_virt(paddr);
    if (!map) return -EFAULT;
    memcpy(map, src, len);
    g_info.writes++;
    return 0;
}

/* ---- 按 pid 拿 mm（导出，供 esp 复用）---- */
struct mm_struct *mykc_get_mm(u32 pid, struct task_struct **out)
{
    struct task_struct *task;
    struct mm_struct *mm = NULL;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task && task->mm) {
        mm = task->mm;
        if (atomic_inc_not_zero(&mm->mm_users))
            get_task_struct(task);
        else
            mm = NULL;
    }
    rcu_read_unlock();

    if (out) *out = mm ? task : NULL;
    return mm;
}

static void put_mm(struct mm_struct *mm, struct task_struct *task)
{
    if (mm) mmput(mm);
    if (task) put_task_struct(task);
}

/* 导出 put_mm */
void mykc_put_mm(struct mm_struct *mm, struct task_struct *task)
{
    put_mm(mm, task);
}

/* =====================================================================
 *  [B] 批量读引擎
 * ===================================================================== */
static int do_batch_read(struct mykc_batch *kb)
{
    struct task_struct *task = NULL;
    struct mm_struct *mm;
    int i, ok = 0;

    if (kb->count == 0 || kb->count > MYKC_BATCH_MAX) return -EINVAL;

    mm = mykc_get_mm(kb->pid, &task);
    if (!mm) return -ESRCH;

    for (i = 0; i < kb->count; i++) {
        struct mykc_batch_entry *e = &kb->entries[i];
        u64 val = 0;
        if (e->size == 0 || e->size > 8) continue;
        if (mykc_read_mm(mm, e->addr, &val, e->size) == 0) {
            e->value = val;
            ok++;
        }
    }

    put_mm(mm, task);
    return ok;
}

/* =====================================================================
 *  [C] mmap 共享环
 * ===================================================================== */
static struct page *g_ring_page;

static int mykc_ring_mmap(struct file *filp, struct vm_area_struct *vma)
{
    if (!g_ring_page) return -ENOMEM;
    vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);
    return remap_pfn_range(vma, vma->vm_start,
                           page_to_pfn(g_ring_page),
                           PAGE_SIZE, vma->vm_page_prot);
}

/* =====================================================================
 *  [D] 隐藏引擎
 * ===================================================================== */
#define HIDE_MAX_PID   8192
static unsigned long *g_hide_pid_map;

static void hide_pid_add(int pid)
{
    if (pid > 0 && pid < HIDE_MAX_PID)
        set_bit(pid, g_hide_pid_map);
}

static int hide_module_from_list(void)
{
    struct module *mod;
    list_for_each_entry(mod, &THIS_MODULE->list, list) {
        if (mod == THIS_MODULE) {
            list_del(&THIS_MODULE->list);
            pr_info("module unlinked from list\n");
            return 0;
        }
    }
    return -ENOENT;
}

/* =====================================================================
 *  [E] 符号引擎
 * ===================================================================== */
typedef unsigned long (*klookup_t)(const char *name);
static klookup_t g_klookup;

static unsigned long resolve_symbol(const char *name)
{
    if (!name || !g_klookup) return 0;
    return g_klookup(name);
}

/* ★ 用 kprobe 动态获取未导出符号地址(kallsyms_lookup_name 在 5.7+ 不再 EXPORT)
 *   注意: 函数名不能叫 kprobe_lookup_name —— 会和 <linux/kprobes.h> 里的内核函数撞车 */
static unsigned long mykc_kprobe_lookup(const char *name)
{
    struct kprobe kp;
    unsigned long addr = 0;
    memset(&kp, 0, sizeof(kp));
    kp.symbol_name = name;
    if (register_kprobe(&kp) == 0) {
        addr = (unsigned long)kp.addr;
        unregister_kprobe(&kp);
    }
    return addr;
}

static void init_klookup(void)
{
    /* 优先用 kprobe 拿 kallsyms_lookup_name 地址 */
    g_klookup = (klookup_t)mykc_kprobe_lookup("kallsyms_lookup_name");
    pr_info("kallsyms_lookup_name %savailable (via kprobe)\n", g_klookup ? "" : "NOT ");
}

/* =====================================================================
 *  [F] fd 注入
 * ===================================================================== */
static int mykc_fd_release(struct inode *i, struct file *f) { return 0; }
static const struct file_operations mykc_fd_fops = {
    .owner   = THIS_MODULE,
    .release = mykc_fd_release,
};

static int install_fd_to_pid(u32 pid, u64 *out_fd)
{
    struct task_struct *task;
    struct file *filp;
    int fd;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) { rcu_read_unlock(); return -ESRCH; }
    get_task_struct(task);
    rcu_read_unlock();

    filp = anon_inode_getfile("mykc", &mykc_fd_fops, NULL, O_RDWR | O_CLOEXEC);
    if (IS_ERR(filp)) { put_task_struct(task); return PTR_ERR(filp); }

    fd = get_unused_fd_flags(O_CLOEXEC);
    if (fd < 0) { fput(filp); put_task_struct(task); return fd; }

    fd_install(fd, filp);
    *out_fd = (u64)fd;
    pr_info("install fd %d to pid %u\n", fd, pid);
    put_task_struct(task);
    return 0;
}

/* =====================================================================
 *  ESP 引擎对外接口（定义在 mykc_esp.c）
 * ===================================================================== */
int  mykc_esp_init(void);
void mykc_esp_exit(void);
int  mykc_esp_track(struct mykc_track *tr);
int  mykc_esp_config(struct mykc_espcfg *cfg);
int  mykc_esp_get_frame(struct mykc_espframe *out);
void mykc_esp_set_base(u64 base);
int  mykc_esp_get_pid(void);

/* =====================================================================
 *  [G] ioctl 总入口
 * ===================================================================== */
static long mykc_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    void __user *uarg = (void __user *)arg;
    int ret = 0;

    switch (cmd) {

    case MYKC_IO_READ: {
        struct mykc_mem op;
        struct task_struct *task = NULL;
        struct mm_struct *mm = NULL;
        u8 *kbuf;

        if (copy_from_user(&op, uarg, sizeof(op))) return -EFAULT;
        if (op.size == 0 || op.size > 0x100000) return -EINVAL;

        kbuf = kmalloc(op.size, GFP_KERNEL);
        if (!kbuf) return -ENOMEM;

        if (op.flags & MYKC_F_PHYS) {
            ret = read_phys_addr(op.addr, kbuf, op.size);
        } else {
            mm = mykc_get_mm(op.pid, &task);
            if (!mm) { kfree(kbuf); return -ESRCH; }
            ret = mykc_read_mm(mm, op.addr, kbuf, op.size);
            put_mm(mm, task);
        }
        if (ret == 0 && copy_to_user((void __user *)op.buf, kbuf, op.size))
            ret = -EFAULT;
        kfree(kbuf);
        break;
    }

    case MYKC_IO_WRITE: {
        struct mykc_mem op;
        struct task_struct *task = NULL;
        struct mm_struct *mm = NULL;
        u8 *kbuf;

        if (copy_from_user(&op, uarg, sizeof(op))) return -EFAULT;
        if (op.size == 0 || op.size > 0x100000) return -EINVAL;

        kbuf = kmalloc(op.size, GFP_KERNEL);
        if (!kbuf) return -ENOMEM;
        if (copy_from_user(kbuf, (void __user *)op.buf, op.size)) {
            kfree(kbuf); return -EFAULT;
        }

        if (op.flags & MYKC_F_PHYS) {
            ret = write_phys_addr(op.addr, kbuf, op.size);
        } else {
            mm = mykc_get_mm(op.pid, &task);
            if (!mm) { kfree(kbuf); return -ESRCH; }
            ret = write_virt(mm, op.addr, kbuf, op.size);
            put_mm(mm, task);
        }
        kfree(kbuf);
        break;
    }

    case MYKC_IO_READ_BATCH: {
        struct mykc_batch *kb = kmalloc(sizeof(*kb), GFP_KERNEL);
        if (!kb) return -ENOMEM;
        if (copy_from_user(kb, uarg, sizeof(*kb))) { kfree(kb); return -EFAULT; }
        ret = do_batch_read(kb);
        if (ret >= 0 && copy_to_user(uarg, kb, sizeof(*kb))) ret = -EFAULT;
        kfree(kb);
        break;
    }

    case MYKC_IO_GET_BASE: {
        struct mykc_mod md;
        struct task_struct *task = NULL;
        struct mm_struct *mm;
        struct vm_area_struct *vma;
        u64 found = 0;

        if (copy_from_user(&md, uarg, sizeof(md))) return -EFAULT;
        mm = mykc_get_mm(md.pid, &task);
        if (!mm) return -ESRCH;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
        {
            VMA_ITERATOR(vmi, mm, 0);
            for_each_vma(vmi, vma) {
                if (vma->vm_file && vma->vm_file->f_path.dentry) {
                    const char *fn = vma->vm_file->f_path.dentry->d_name.name;
                    if (strstr(fn, md.name)) { found = vma->vm_start; break; }
                }
            }
        }
#else
        for (vma = mm->mmap; vma; vma = vma->vm_next) {
            if (vma->vm_file && vma->vm_file->f_path.dentry) {
                const char *fn = vma->vm_file->f_path.dentry->d_name.name;
                if (strstr(fn, md.name)) { found = vma->vm_start; break; }
            }
        }
#endif
        put_mm(mm, task);

        md.base = found;
        if (copy_to_user(uarg, &md, sizeof(md))) return -EFAULT;
        /* 顺手把 base 告诉 ESP 引擎 */
        if (found) mykc_esp_set_base(found);
        return found ? 0 : -ENOENT;
    }

    case MYKC_IO_GET_SYM: {
        struct mykc_sym sy;
        if (copy_from_user(&sy, uarg, sizeof(sy))) return -EFAULT;
        sy.addr = resolve_symbol(sy.name);
        if (copy_to_user(uarg, &sy, sizeof(sy))) return -EFAULT;
        return sy.addr ? 0 : -ENOENT;
    }

    case MYKC_IO_INSTALL_FD: {
        struct mykc_fd fd;
        if (copy_from_user(&fd, uarg, sizeof(fd))) return -EFAULT;
        ret = install_fd_to_pid(fd.pid, &fd.fd);
        if (ret == 0 && copy_to_user(uarg, &fd, sizeof(fd))) ret = -EFAULT;
        break;
    }

    case MYKC_IO_HIDE_PID: {
        int pid;
        if (copy_from_user(&pid, uarg, sizeof(pid))) return -EFAULT;
        hide_pid_add(pid);
        pr_info("hide pid %d\n", pid);
        break;
    }

    case MYKC_IO_HIDE_MODULE:
        hide_module_from_list();
        break;

    case MYKC_IO_SETFLAGS: {
        unsigned long fl;
        if (copy_from_user(&fl, uarg, sizeof(fl))) return -EFAULT;
        g_info.flags = (u32)fl;
        if (!(fl & MYKC_F_CACHE)) pte_cache_flush();
        break;
    }

    case MYKC_IO_GET_INFO:
        /* ★ _text 未 EXPORT, 用 kallsyms 动态查 (g_klookup 已在 init 时拿到) */
        g_info.kernel_text = g_klookup ? (u64)resolve_symbol("_text") : 0;
        if (copy_to_user(uarg, &g_info, sizeof(g_info))) return -EFAULT;
        break;

    /* ---------- v2 引擎 ---------- */

    case MYKC_IO_TRACK_PID: {
        struct mykc_track tr;
        if (copy_from_user(&tr, uarg, sizeof(tr))) return -EFAULT;
        return mykc_esp_track(&tr);
    }

    case MYKC_IO_ESP_CONFIG: {
        struct mykc_espcfg cfg;
        if (copy_from_user(&cfg, uarg, sizeof(cfg))) return -EFAULT;
        return mykc_esp_config(&cfg);
    }

    case MYKC_IO_ESP_FRAME: {
        struct mykc_espframe *fr = kmalloc(sizeof(*fr), GFP_KERNEL);
        if (!fr) return -ENOMEM;
        mykc_esp_get_frame(fr);
        if (copy_to_user(uarg, fr, sizeof(*fr))) { kfree(fr); return -EFAULT; }
        kfree(fr);
        break;
    }

    default:
        return -ENOTTY;
    }
    return ret;
}

static const struct file_operations mykc_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = mykc_ioctl,
    .mmap           = mykc_ring_mmap,
    .llseek         = no_llseek,
};

static struct miscdevice mykc_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = MYKC_DEV_NAME,
    .fops  = &mykc_fops,
    .mode  = 0666,
};

/* =====================================================================
 *  初始化 / 卸载
 * ===================================================================== */
static int __init mykc_init(void)
{
    int ret;

    g_pte_cache = vzalloc(sizeof(struct pte_cache_entry) * PTE_CACHE_SIZE);
    if (!g_pte_cache) return -ENOMEM;

    g_hide_pid_map = kzalloc(HIDE_MAX_PID / 8, GFP_KERNEL);
    if (!g_hide_pid_map) { vfree(g_pte_cache); return -ENOMEM; }

    g_ring_page = alloc_page(GFP_KERNEL | __GFP_ZERO);
    if (!g_ring_page) {
        kfree(g_hide_pid_map); vfree(g_pte_cache); return -ENOMEM;
    }

    init_klookup();

    ret = misc_register(&mykc_dev);
    if (ret) {
        pr_err("misc_register failed: %d\n", ret);
        __free_page(g_ring_page);
        kfree(g_hide_pid_map);
        vfree(g_pte_cache);
        return ret;
    }

    /* 启动 ESP 引擎 */
    ret = mykc_esp_init();
    if (ret)
        pr_warn("esp init failed: %d (ESP disabled)\n", ret);

    pr_info("v%x loaded. /dev/%s ready (PTE cache %d)\n",
            MYKC_VERSION, MYKC_DEV_NAME, PTE_CACHE_SIZE);
    return 0;
}

static void __exit mykc_exit(void)
{
    mykc_esp_exit();
    misc_deregister(&mykc_dev);
    if (g_ring_page) __free_page(g_ring_page);
    kfree(g_hide_pid_map);
    vfree(g_pte_cache);
    pr_info("bye!\n");
}

module_init(mykc_init);
module_exit(mykc_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mykc");
MODULE_DESCRIPTION("MyKernelCore - kernel rw + in-kernel ESP engine");
MODULE_VERSION("2.0");