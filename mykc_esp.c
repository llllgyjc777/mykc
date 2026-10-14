/* mykc_esp.c —— 内核态 ESP 引擎（MyKernelCore v2 核心）
 *
 * 原创点 vs paradise:
 *   paradise:  App 轮询 → 读内存 → 用户态算 W2S → Overlay 绘制
 *   mykc:      内核定时器 → 内核读内存 → 内核算 W2S → 直接吐屏幕方框
 *              App 只负责「画」，一个字节的游戏数据都不碰
 *
 * 三大组件:
 *   [1] tracker   —— 记住游戏进程（自动认领，App 免传 PID）
 *   [2] scanner   —— 内核定时器驱动 + 类缓存 + 每 30 帧重扫
 *   [3] projector —— 内核态 W2S，产出屏幕方框
 */
#define pr_fmt(fmt) "[mykc-esp] " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/ktime.h>
#include <linux/math.h>
#include <linux/sched/signal.h>
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#include <linux/pid_namespace.h>

#include "mykc.h"

/* core 提供的接口 */
int  mykc_read_mm(struct mm_struct *mm, u64 vaddr, void *dst, size_t len);
struct mm_struct *mykc_get_mm(u32 pid, struct task_struct **task);
void mykc_put_mm(struct mm_struct *mm, struct task_struct *task);

/* =====================================================================
 *  偏移常量（来自 espcalc.c 实战验证）
 * ===================================================================== */
#define OFF_GWORLD       0x1771CC38ULL
#define OFF_LEVEL        0xB8ULL
#define OFF_ACTORS       0xA0ULL
#define OFF_ACTOR_COUNT  0xA8ULL
#define OFF_ACTOR_CLS    0x10ULL
#define OFF_ACTOR_ROOT   0x260ULL
#define OFF_ROOT_POS     0x200ULL
#define OFF_ACTOR_HP     0x1060ULL
#define OFF_ACTOR_POV    0x14B0ULL
#define OFF_POV_FOV      0x30ULL

#define PLAYER_HALF      90.0f
#define SHOULDER_RATIO   0.35f
#define MAX_ITER         20000
#define PI_F             3.14159265f
#define D2R              (PI_F / 180.0f)

/* =====================================================================
 *  全局状态
 * ===================================================================== */
struct esp_cls { u64 cls; int cnt; };

static struct {
    /* 追踪 */
    u32   game_pid;
    u32   track_flags;
    char  game_pkg[64];
    u64   game_base;

    /* 配置 */
    int   enable;
    int   interval_ms;
    int   screen_w, screen_h;
    u32   max_dist;
    u64   gworld_off;
    u64   fov;

    /* 帧（双缓冲）*/
    struct mykc_espframe *frame_front;
    struct mykc_espframe *frame_back;
    spinlock_t frame_lock;
    u32   seq;

    /* 类缓存 */
    struct esp_cls *classes;
    int   cls_count;
    int   cls_capacity;
    u64   cached_cam_pov;
    u64   cached_player_cls;
    int   frame_counter;

    /* 定时器 */
    struct timer_list timer;
    struct work_struct work;
    int   work_queued;

    /* 统计 */
    u64   frames_done;
} g_esp;

/* =====================================================================
 *  读内存辅助
 * ===================================================================== */
static int esp_read(struct mm_struct *mm, u64 addr, void *buf, size_t n)
{
    if (!addr || addr < 0x10000ULL || (addr >> 63)) return -EFAULT;
    return mykc_read_mm(mm, addr, buf, n);
}

static u64 esp_rp(struct mm_struct *mm, u64 addr)
{
    u64 v = 0;
    esp_read(mm, addr, &v, 8);
    return v;
}

static u32 esp_ru32(struct mm_struct *mm, u64 addr)
{
    u32 v = 0;
    esp_read(mm, addr, &v, 4);
    return v;
}

static float esp_rf(struct mm_struct *mm, u64 addr)
{
    float v = 0.0f;
    esp_read(mm, addr, &v, 4);
    return v;
}

static int esp_readable(struct mm_struct *mm, u64 addr)
{
    char b[8];
    if (!addr || addr < 0x10000ULL || (addr >> 63)) return 0;
    return esp_read(mm, addr, b, 8) == 0;
}

/* =====================================================================
 *  内核态 W2S
 * ===================================================================== */
typedef struct { float x, y, z; } V3;
typedef struct {
    V3 loc;
    float pitch, yaw, roll, fov;
    int W, H;
} EspCam;

static int esp_w2s(const EspCam *c, V3 t, float *sx, float *sy)
{
    float dx, dy, dz, cp, sp, cy, syw;
    float Fx, Fy, Fz, Ux, Uy, Uz;
    float xr, yr, zr, focal;

    dx = t.x - c->loc.x;
    dy = t.y - c->loc.y;
    dz = t.z - c->loc.z;

    cp = cosf(c->pitch * D2R);
    sp = sinf(c->pitch * D2R);
    cy = cosf(c->yaw * D2R);
    syw = sinf(c->yaw * D2R);

    Fx = cp * cy;  Fy = cp * syw;  Fz = sp;
    Ux = -sp * cy; Uy = -sp * syw; Uz = cp;

    xr = dx * (-syw) + dy * cy;
    yr = dx * Ux + dy * Uy + dz * Uz;
    zr = dx * Fx + dy * Fy + dz * Fz;

    if (zr < 1.0f) return 0;

    focal = ((float)c->W * 0.5f) / tanf(c->fov * 0.5f * D2R);
    *sx = (float)c->W * 0.5f + (xr / zr) * focal;
    *sy = (float)c->H * 0.5f - (yr / zr) * focal;
    return 1;
}

static void esp_box_of(const EspCam *c, V3 foot,
                       float *x0, float *y0, float *x1, float *y1)
{
    V3 bot = { foot.x, foot.y, foot.z - PLAYER_HALF };
    V3 top = { foot.x, foot.y, foot.z + PLAYER_HALF };
    float sx, sy, hx, hy, h, w;

    if (!esp_w2s(c, bot, &sx, &sy)) { *x0 = *y0 = *x1 = *y1 = -9999; return; }
    if (!esp_w2s(c, top, &hx, &hy)) { *x0 = *y0 = *x1 = *y1 = -9999; return; }

    h = sy - hy; if (h < 0) h = -h; if (h < 2) h = 2;
    w = h * SHOULDER_RATIO;

    *x0 = sx - w * 0.5f;
    *y0 = hy;
    *x1 = sx + w * 0.5f;
    *y1 = sy;
}

/* 找相机 POV */
static u64 esp_find_cam(struct mm_struct *mm, u64 arr, int n)
{
    int i;
    for (i = 0; i < n && i < MAX_ITER; i++) {
        u64 a = esp_rp(mm, arr + (u64)i * 8);
        u64 pov;
        float x, y, z, p, yy, r, fv;

        if (!esp_readable(mm, a)) continue;
        pov = a + OFF_ACTOR_POV;
        x  = esp_rf(mm, pov);
        y  = esp_rf(mm, pov + 4);
        z  = esp_rf(mm, pov + 8);
        p  = esp_rf(mm, pov + 0x18);
        yy = esp_rf(mm, pov + 0x1C);
        r  = esp_rf(mm, pov + 0x20);
        fv = esp_rf(mm, pov + OFF_POV_FOV);

        if (x > 1e5f && x < 1.5e6f && y > 1e5f && y < 1.5e6f &&
            z > 500 && z < 5000 &&
            (p < 0 ? -p : p) <= 89.5f &&
            (yy < 0 ? -yy : yy) <= 360.5f &&
            (r < 0 ? -r : r) <= 1.5f &&
            fv >= 50 && fv <= 110)
            return pov;
    }
    return 0;
}

/* =====================================================================
 *  类分组缓存
 * ===================================================================== */
static void esp_cls_reset(void) { g_esp.cls_count = 0; }

static void esp_cls_add(u64 cls)
{
    int i;
    for (i = 0; i < g_esp.cls_count; i++) {
        if (g_esp.classes[i].cls == cls) {
            g_esp.classes[i].cnt++;
            return;
        }
    }
    if (g_esp.cls_count < g_esp.cls_capacity) {
        g_esp.classes[g_esp.cls_count].cls = cls;
        g_esp.classes[g_esp.cls_count].cnt = 1;
        g_esp.cls_count++;
    }
}

/* 选最可能的人类类 */
static u64 esp_pick_player_cls(struct mm_struct *mm, u64 arr, int n)
{
    int i, best = -1, best_hits = -1;

    for (i = 0; i < g_esp.cls_count; i++) {
        int hits = 0, valid = 0, k;

        if (g_esp.classes[i].cnt < 3 || g_esp.classes[i].cnt > 200) continue;

        for (k = 0; k < n && valid < 60 && k < MAX_ITER; k++) {
            u64 a = esp_rp(mm, arr + (u64)k * 8);
            float h;
            if (!esp_readable(mm, a)) continue;
            if (esp_rp(mm, a + OFF_ACTOR_CLS) != g_esp.classes[i].cls) continue;
            valid++;
            h = esp_rf(mm, a + OFF_ACTOR_HP);
            if (h >= 1.0f && h <= 100.0f) hits++;
        }
        if (hits > best_hits) { best_hits = hits; best = i; }
    }
    return (best >= 0) ? g_esp.classes[best].cls : 0;
}

/* =====================================================================
 *  单帧扫描
 * ===================================================================== */
static int esp_scan_frame(struct mm_struct *mm, struct mykc_espframe *out)
{
    u64 base = g_esp.game_base;
    u64 gworld_off = g_esp.gworld_off ? g_esp.gworld_off : OFF_GWORLD;
    u64 gworld, level, arr, cam_pov, player_cls;
    EspCam cam;
    int n, i, box_n = 0, reScan;

    out->count = 0;
    out->seq = ++g_esp.seq;
    out->ts_ns = ktime_get_ns();
    out->game_pid = g_esp.game_pid;

    gworld = esp_rp(mm, base + gworld_off);
    if (!esp_readable(mm, gworld)) return -EAGAIN;
    level = esp_rp(mm, gworld + OFF_LEVEL);
    if (!esp_readable(mm, level)) return -EAGAIN;
    arr = esp_rp(mm, level + OFF_ACTORS);
    n = (int)esp_ru32(mm, level + OFF_ACTOR_COUNT);
    if (!esp_readable(mm, arr) || n <= 0 || n > MAX_ITER) return -EAGAIN;

    reScan = ((g_esp.frame_counter++ % 30) == 0) ||
             !g_esp.cached_cam_pov || !g_esp.cached_player_cls;

    if (reScan) {
        esp_cls_reset();
        for (i = 0; i < n && i < MAX_ITER; i++) {
            u64 a = esp_rp(mm, arr + (u64)i * 8);
            u64 cls;
            if (!esp_readable(mm, a)) continue;
            cls = esp_rp(mm, a + OFF_ACTOR_CLS);
            if (!esp_readable(mm, cls)) continue;
            esp_cls_add(cls);
        }
        g_esp.cached_cam_pov = esp_find_cam(mm, arr, n);
        g_esp.cached_player_cls = esp_pick_player_cls(mm, arr, n);
    }

    cam_pov = g_esp.cached_cam_pov;
    player_cls = g_esp.cached_player_cls;
    if (!cam_pov || !player_cls) return -EAGAIN;

    cam.loc.x = esp_rf(mm, cam_pov);
    cam.loc.y = esp_rf(mm, cam_pov + 4);
    cam.loc.z = esp_rf(mm, cam_pov + 8);
    cam.pitch = esp_rf(mm, cam_pov + 0x18);
    cam.yaw   = esp_rf(mm, cam_pov + 0x1C);
    cam.roll  = esp_rf(mm, cam_pov + 0x20);
    cam.fov   = esp_rf(mm, cam_pov + OFF_POV_FOV);
    cam.W = g_esp.screen_w;
    cam.H = g_esp.screen_h;
    if (cam.fov < 50 || cam.fov > 110) cam.fov = 80.0f;

    for (i = 0; i < n && i < MAX_ITER && box_n < MYKC_BOX_MAX; i++) {
        u64 a = esp_rp(mm, arr + (u64)i * 8);
        u64 cls, root;
        V3 pos;
        float hp, x0, y0, x1, y1;
        float dx, dy, dz, dist_m;

        if (!esp_readable(mm, a)) continue;
        cls = esp_rp(mm, a + OFF_ACTOR_CLS);
        if (cls != player_cls) continue;

        hp = esp_rf(mm, a + OFF_ACTOR_HP);
        if (hp <= 0.0f || hp > 100.0f) continue;

        root = esp_rp(mm, a + OFF_ACTOR_ROOT);
        if (!esp_readable(mm, root)) continue;

        pos.x = esp_rf(mm, root + OFF_ROOT_POS);
        pos.y = esp_rf(mm, root + OFF_ROOT_POS + 4);
        pos.z = esp_rf(mm, root + OFF_ROOT_POS + 8);
        if (pos.z < -1000.0f || pos.z > 1e5f) continue;

        dx = pos.x - cam.loc.x;
        dy = pos.y - cam.loc.y;
        dz = pos.z - cam.loc.z;
        dist_m = sqrtf(dx*dx + dy*dy + dz*dz) / 100.0f;

        if (g_esp.max_dist && dist_m > (float)g_esp.max_dist) continue;

        esp_box_of(&cam, pos, &x0, &y0, &x1, &y1);
        if (x0 <= -9000) continue;

        out->boxes[box_n].x0 = x0;
        out->boxes[box_n].y0 = y0;
        out->boxes[box_n].x1 = x1;
        out->boxes[box_n].y1 = y1;
        out->boxes[box_n].dist = dist_m;
        out->boxes[box_n].hp = hp;
        out->boxes[box_n].flags = MYKC_BOX_VISIBLE;
        box_n++;
    }

    out->count = box_n;
    g_esp.frames_done++;
    return 0;
}

/* =====================================================================
 *  定时器 + 工作队列
 * ===================================================================== */
static void esp_work_fn(struct work_struct *w)
{
    struct task_struct *task = NULL;
    struct mm_struct *mm;
    struct mykc_espframe *back;

    g_esp.work_queued = 0;

    if (!g_esp.enable || !g_esp.game_pid || !g_esp.game_base) return;

    mm = mykc_get_mm(g_esp.game_pid, &task);
    if (!mm) {
        g_esp.game_base = 0;   /* 游戏没了，等重新认领 */
        return;
    }

    back = g_esp.frame_back;
    if (back && esp_scan_frame(mm, back) == 0) {
        struct mykc_espframe *tmp;
        spin_lock(&g_esp.frame_lock);
        tmp = g_esp.frame_front;
        g_esp.frame_front = g_esp.frame_back;
        g_esp.frame_back = tmp;
        spin_unlock(&g_esp.frame_lock);
    }
    mykc_put_mm(mm, task);
}

static void esp_timer_fn(struct timer_list *t)
{
    unsigned long interval;

    if (g_esp.enable && g_esp.game_pid && !g_esp.work_queued) {
        g_esp.work_queued = 1;
        schedule_work(&g_esp.work);
    }

    interval = msecs_to_jiffies(g_esp.interval_ms > 0 ? g_esp.interval_ms : 8);
    mod_timer(&g_esp.timer, jiffies + interval);
}

/* =====================================================================
 *  对外接口
 * ===================================================================== */
int mykc_esp_init(void)
{
    memset(&g_esp, 0, sizeof(g_esp));

    spin_lock_init(&g_esp.frame_lock);
    g_esp.interval_ms = 8;
    g_esp.screen_w = 3200;
    g_esp.screen_h = 1440;
    g_esp.gworld_off = OFF_GWORLD;
    g_esp.fov = 80;

    g_esp.frame_front = vzalloc(sizeof(struct mykc_espframe));
    g_esp.frame_back  = vzalloc(sizeof(struct mykc_espframe));
    g_esp.cls_capacity = 4096;
    g_esp.classes = vzalloc(sizeof(struct esp_cls) * g_esp.cls_capacity);

    if (!g_esp.frame_front || !g_esp.frame_back || !g_esp.classes) {
        pr_err("alloc failed\n");
        return -ENOMEM;
    }

    INIT_WORK(&g_esp.work, esp_work_fn);
    timer_setup(&g_esp.timer, esp_timer_fn, 0);
    mod_timer(&g_esp.timer, jiffies + msecs_to_jiffies(100));

    pr_info("engine v2 init (interval=%dms, %dx%d)\n",
            g_esp.interval_ms, g_esp.screen_w, g_esp.screen_h);
    return 0;
}

void mykc_esp_exit(void)
{
    del_timer_sync(&g_esp.timer);
    cancel_work_sync(&g_esp.work);
    if (g_esp.frame_front) vfree(g_esp.frame_front);
    if (g_esp.frame_back)  vfree(g_esp.frame_back);
    if (g_esp.classes)     vfree(g_esp.classes);
    pr_info("engine exit (frames=%llu)\n", g_esp.frames_done);
}

int mykc_esp_track(struct mykc_track *tr)
{
    if (tr->pid == 0) {
        g_esp.game_pid = 0;
        g_esp.game_base = 0;
        g_esp.cached_cam_pov = 0;
        g_esp.cached_player_cls = 0;
        pr_info("untrack\n");
        return 0;
    }
    g_esp.game_pid = tr->pid;
    g_esp.track_flags = tr->flags;
    if (tr->pkg[0])
        strscpy(g_esp.game_pkg, tr->pkg, sizeof(g_esp.game_pkg));
    pr_info("track pid=%u pkg=%s\n", g_esp.game_pid, g_esp.game_pkg);
    return 0;
}

void mykc_esp_set_base(u64 base)
{
    g_esp.game_base = base;
    pr_info("base set to 0x%llx\n", base);
}

int mykc_esp_config(struct mykc_espcfg *cfg)
{
    g_esp.enable = cfg->enable ? 1 : 0;
    if (cfg->interval_ms)  g_esp.interval_ms = cfg->interval_ms;
    if (cfg->screen_w)     g_esp.screen_w = cfg->screen_w;
    if (cfg->screen_h)     g_esp.screen_h = cfg->screen_h;
    g_esp.max_dist = cfg->max_dist;
    if (cfg->gworld_off)   g_esp.gworld_off = cfg->gworld_off;
    if (cfg->fov)          g_esp.fov = cfg->fov;
    pr_info("config: enable=%d interval=%d %dx%d maxdist=%u\n",
            g_esp.enable, g_esp.interval_ms, g_esp.screen_w,
            g_esp.screen_h, g_esp.max_dist);
    return 0;
}

int mykc_esp_get_frame(struct mykc_espframe *out)
{
    spin_lock(&g_esp.frame_lock);
    memcpy(out, g_esp.frame_front, sizeof(struct mykc_espframe));
    spin_unlock(&g_esp.frame_lock);
    return 0;
}

int mykc_esp_get_pid(void) { return g_esp.game_pid; }