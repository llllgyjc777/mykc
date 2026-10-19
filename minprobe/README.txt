========================================
MYKC 最小装载验证包
========================================

【目的】
判断 .ko 装载失败到底出在哪一层：

  STEP0 崩溃  ->  编译链 / CRC / 工具链问题（代码怎么改都没用）
  STEP0 通过  ->  编译链正常，问题在 mykc 具体代码

用 6 个逐级放开功能的模块做二分定位，避免继续盲改代码。

------------------------------------------------
【文件说明】

mp.c            最小验证模块源码
                用 MP_STEP 宏控制功能层级：
                  STEP 0  只有 pr_info，init 直接返回 0
                  STEP 1  + vzalloc 16KB / vfree
                  STEP 2  + kzalloc 1KB / kfree
                  STEP 3  + alloc_page / __free_page
                  STEP 4  + misc_register / misc_deregister
                  STEP 5  + register_kprobe(report_cfi_failure)  ← 头号嫌疑

Makefile        本地 / 传统内核树编译
                make STEP=3
                make verify      # 打印 vermagic、kCFI、符号、md5

build_mp.yml    GitHub Actions 云编译，一次编出 mp_s0~mp_s5 六个 .ko
                放到仓库 .github/workflows/ 下，手动触发

kadeflow.yaml   kade 框架配置（systemnb/compile_android_driver）
                走 GKI 官方编译流程 + 自动 ABI 对齐

abi.symbols     kade 用的 ABI 符号清单
                声明所需符号后自动导出，解决 CRC 对不上

loadtest.sh     设备端逐级装载测试脚本
                sh loadtest.sh        # 从 STEP0 测到 STEP5
                sh loadtest.sh 3      # 从 STEP3 开始

------------------------------------------------
【使用步骤】

方案 A：先用现有 mykc 仓库的云编译（最快）

  1. 把 mp.c / Makefile 传到仓库（可新建 minprobe/ 目录）
  2. 把 build_mp.yml 放到 .github/workflows/
  3. GitHub Actions 手动触发 Build Minimal Probe
  4. 下载 artifact: mp-probes（含 mp_s0.ko ~ mp_s5.ko）
  5. 传到设备：
       adb push mp_s*.ko /data/local/tmp/
       adb push loadtest.sh /data/local/tmp/
       adb shell su -c "sh /data/local/tmp/loadtest.sh"
  6. 看 STEP0 是否通过

方案 B：换 kade 编译（编译链更规范）

  1. 建目录：code/ 放 mp.c + Makefile
  2. abi.symbols / kadeflow.yaml 放仓库根
  3. 把 kade.yml 放 .github/workflows/
  4. 触发云编译

------------------------------------------------
【关键判据】

看 dmesg 里有没有这几行：

  mp: loaded, step=0, ver=MP_BUILD_v1_STEP0
  mp: init done, returning 0

有  ->  STEP0 通过，编译链 OK
      ->  继续测 STEP1..5，找出哪一步开始崩

没有，且设备重启
    ->  STEP0 就崩了
    ->  问题在编译链层面：vermagic / CRC / clang 版本 / CFI 编译模式
    ->  此时应该换 kade 流程重编，而不是继续改 mykc 代码

------------------------------------------------
【当前已知情报】

设备：Redmi K80 Pro (miro / SM8750)
      Android 16 / SDK 36
      6.6.118-android15-8-gc44b714366cc-abogki519650608-4k
      KernelSU 3.3.1

内核配置关键项：
  CONFIG_CFI_CLANG=y              ← 严格模式
  # CONFIG_CFI_PERMISSIVE is not set
  CONFIG_ARM64_PTR_AUTH=y
  CONFIG_ARM64_BTI=y
  # CONFIG_ARM64_BTI_KERNEL is not set
  CONFIG_SHADOW_CALL_STACK=y
  CONFIG_MODVERSIONS=y
  CONFIG_MODULE_SIG=y
  # CONFIG_MODULE_SIG_FORCE is not set   ← 不强制签名，可绕过
  # CONFIG_LTO_CLANG_THIN is not set
  # CONFIG_LTO_CLANG_FULL is not set

已实测对照：
  paradise66.ko   __kcfi_typeid = 28   可装载
  unfairmem.ko    __kcfi_typeid = 3    可装载
  我们的全部 .ko  __kcfi_typeid = 0

⚠ 注意：__kcfi_typeid 数量只是表象。
  实测 clang -fsanitize=kcfi 会生成 kCFI 检查指令
  （ldur w16,[x2,#-4] / cmp / b.eq / brk #0x8222），
  但 typeid 符号只在跨模块取函数地址时才产生。
  所以判据应看「brk #0x8222 是否存在于 .text」，而非 typeid 数量。

========================================