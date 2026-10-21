/* mykc.h —— MyKernelCore 共享协议头（内核模块 + 用户态 App 共用）
 *
 * 原创设计目标（对比 paradise 的优化点）：
 *   1. 页表遍历缓存 PTE-Cache    → 重复读同一页命中缓存，省 2 次页表走查
 *   2. 批量读 batch_read         → 一次 ioctl 读多个散地址，省 syscall 开销
 *   3. mmap 共享环 ring buffer   → 零拷贝，App 直接读映射内存，不走 ioctl
 * *   5. 自动追踪进程              → 内核自己认领游戏，App 免传 PID
 */
#ifndef _MYKC_H
#define _MYKC_H

#include <linux/types.h>

#define MYKC_MAGIC        0x4d4b4301u   /* "MKC\1" */
#define MYKC_DEV_NAME     "mykc"
#define MYKC_VERSION      0x0300        /* 3.0: 纯内存原语 + 隐蔽 */

/* =====================================================================
 *  结构体定义（必须在 ioctl 宏之前）
 * ===================================================================== */

/* ---------- 数据通道 1: 单次读写 ---------- */
struct mykc_mem {
    __u64 addr;      /* 目标进程虚拟地址（PHYS 模式时为物理地址） */
    __u64 buf;       /* 用户态缓冲区 */
    __u64 size;
    __u32 pid;       /* 目标进程 pid；0 = 用 addr 当物理地址 */
    __u32 flags;     /* MYKC_F_* */
};
#define MYKC_F_PHYS       0x0001
#define MYKC_F_CACHE      0x0002
#define MYKC_F_NOCACHE    0x0004

/* ---------- 数据通道 2: 批量读 ---------- */
#define MYKC_BATCH_MAX    64
struct mykc_batch_entry {
    __u64 addr;
    __u32 size;      /* <= 8 */
    __u32 _pad;
    __u64 value;     /* 读出值（小端） */
};
struct mykc_batch {
    __u32 pid;
    __u32 count;
    struct mykc_batch_entry entries[MYKC_BATCH_MAX];
};

/* ---------- 模块基址 ---------- */
struct mykc_mod {
    __u32  pid;
    __u32  _pad;
    __u64  base;            /* 输出：模块加载基址 */
    char   name[32];        /* 输入：模块名，如 "libUE4.so" */
};

/* ---------- 符号解析 ---------- */
struct mykc_sym {
    __u64  addr;            /* 输出：符号地址 */
    char   name[64];        /* 输入：符号名 */
};

/* ---------- v34: 按名字查进程 ---------- */
struct mykc_findproc {
    char  name[64];      /* 输入：comm 名字片段，如 "pubgmhd" */
    __u32 out_pid;       /* 输出：找到的 pid，0 = 未找到 */
    __u32 _pad;
};

/* ---------- fd 注入 ---------- */
struct mykc_fd {
    __u32  pid;
    __u32  _pad;
    __u64  fd;              /* 输出：注入的 fd 号 */
};

/* ---------- 信息查询 ---------- */
struct mykc_info {
    __u32  version;
    __u32  flags;
    __u32  pte_cache_hits;
    __u32  pte_cache_miss;
    __u64  reads;
    __u64  writes;
    __u64  kernel_text;
};
#define MYKC_TRK_PIN        0x0001   /* 固定追踪 */
#define MYKC_TRK_AUTO       0x0002   /* 自动认领 */

/* ---------- v2: 方框 ---------- */
/* ★ 内核禁浮点: 全部用 s32。坐标=像素; dist=米; hp=值×100 */
#define MYKC_BOX_MAX      64
struct mykc_box {
    __s32  x0, y0, x1, y1;  /* 屏幕坐标（像素，整数） */
    __s32  dist;            /* 距离（米） */
    __s32  hp;              /* 血量 ×100 */
    __u32  flags;           /* MYKC_BOX_* */
};
#define MYKC_BOX_VISIBLE    0x0001
#define MYKC_BOX_CACHED     0x0002

/* =====================================================================
 *  ioctl 命令（结构体之后定义，避免未声明）
 * ===================================================================== */
#define MYKC_IO_READ          _IOWR(MYKC_MAGIC, 1,  struct mykc_mem)
#define MYKC_IO_WRITE         _IOWR(MYKC_MAGIC, 2,  struct mykc_mem)
#define MYKC_IO_READ_BATCH    _IOWR(MYKC_MAGIC, 3,  struct mykc_batch)
#define MYKC_IO_GET_BASE      _IOWR(MYKC_MAGIC, 4,  struct mykc_mod)
#define MYKC_IO_GET_SYM       _IOWR(MYKC_MAGIC, 5,  struct mykc_sym)
#define MYKC_IO_INSTALL_FD    _IOWR(MYKC_MAGIC, 6,  struct mykc_fd)
#define MYKC_IO_HIDE_PID      _IOW(MYKC_MAGIC,  7,  int)
#define MYKC_IO_HIDE_MODULE   _IOW(MYKC_MAGIC,  8,  int)
#define MYKC_IO_SETFLAGS      _IOW(MYKC_MAGIC,  9,  unsigned long)
#define MYKC_IO_GET_INFO      _IOR(MYKC_MAGIC,  10, struct mykc_info)
/* ★ v4: 隐身模式（S1 摘 lsmod / S2 摘 sysfs / S3 焊死 /proc/devices 入口） */
#define MYKC_IO_STEALTH       _IOW(MYKC_MAGIC,  14, unsigned long)
/* v34: 按名字查进程 */
#define MYKC_IO_FIND_PROC     _IOWR(MYKC_MAGIC, 15, struct mykc_findproc)

/* 隐身等级（数字越大越隐蔽；0=全公开，调试用） */
#define MYKC_STEALTH_NONE     0u  /* 全公开 */
#define MYKC_STEALTH_LIST     1u  /* 仅从 lsmod 摘除 */
#define MYKC_STEALTH_SYSFS    2u  /* 再摘 /sys/module */
#define MYKC_STEALTH_FULL     3u  /* 再注销 misc 设备（/proc/devices 无痕，fd 仍可用） */

/* ---------- 用户态 mmap 共享环 ---------- */
#define MYKC_RING_SIZE    (64 * 1024)
#define MYKC_RING_MAGIC   0x52494e47u     /* "RING" */

struct mykc_ring_hdr {
    __u32  magic;
    __u32  head;
    __u32  tail;
    __u32  seq;
    __u32  data_len;
    __u32  _pad[3];
    __u8   data[MYKC_RING_SIZE - 32];
};

#endif /* _MYKC_H */