# Makefile —— MyKernelCore 内核模块
# 目标内核: 6.6.118-android15-8-GKI (K80 Pro)
#
# 方式 A（推荐，GKI 官方方式）: 用 KMI 模块符号表编译
#   需要: kernel_source_tree/ (含 Module.symvers) + clang 18
# 方式 B: 用完整内核源码树

obj-m += mykc_core.o
mykc_core-objs := mykc_main.o mykc_esp.o
# ★ A 路线 (2026-10-16): mykc_cfi.o 提供 CFI 严格模式绕过
#   原理: patch report_cfi_failure -> RET
#   实证: 设备 CONFIG_CFI_CLANG=y 且 CFI_PERMISSIVE 未开; 设备内核自己
#         也没开 LTO_CLANG_THIN => 证明 typeid 与 LTO 无关, 路线 B 是死路。
# ★ 对照实验用最小模块（验证编译环境是否能产出可 insmod 的 .ko）
#   2026-10-16: 暂时关掉, 避免干扰主产物; 需要时再打开
# obj-m += hello_min.o
# 说明: mykc_main.c 是主体(ioctl/内存引擎), mykc_esp.c 是 ESP 引擎
#       二者一起链接成单个 mykc_core.ko
#       注意: 主体文件必须叫 mykc_main.c（不能叫 mykc_core.c，否则和 obj-m 冲突）

# ---- 目标内核参数 ----
ARCH            ?= arm64
CROSS_COMPILE   ?= $(HOME)/toolchains/clang/bin/aarch64-linux-gnu-
KDIR            ?= $(PWD)/kernel_source_tree
CLANG_TRIPLE    ?= aarch64-linux-gnu-
CC              ?= clang
LD              ?= ld.lld
LLVM            ?= $(HOME)/toolchains/clang/bin

# GKI 内核编译必须参数
#
# ★★★ 路线 B 核心 (2026-10-16): 必须开 kCFI ★★★
#   设备内核 CONFIG_CFI_CLANG=y 且 CFI_PERMISSIVE 未开 => 严格模式。
#   不生成 __kcfi_typeid_* 的模块 => 间接调用被 check_cfi 拦 => BUG => 秒重启。
#   (对照 paradise66.ko: 它有 28 个 __kcfi_typeid_* 符号 => 能活)
#
#   两种传法同时上(冗余保险):
#     1) workflow 的 make 命令行传 KCFLAGS="-fsanitize=kcfi"
#     2) 这里在 ccflags-y 里也加一份
#
#   -fno-sanitize-cfi-canonical-jump-tables: GKI 常用, 避免跳转表 canonical 化
EXTRA_CFLAGS := -Wno-unused-variable -Wno-unused-function -Wno-format

# 模块级 flag (ccflags-y 对这个 obj-m 下所有 .c 生效)
# ★ 只留 -fsanitize=kcfi (clang-r614150 已不需要 canonical-jump-tables 开关;
#   多了会因 -Werror,-Wunused-command-line-argument 直接编挂 —— 实证 run #56)
# ★ 再补一个 -Wno-unused-command-line-argument 兜底, 防止其他 flag 重复时报错
ccflags-y += -fsanitize=kcfi
ccflags-y += -Wno-unused-command-line-argument

all:
	@echo "=== 编译 mykc_core.ko ==="
	$(MAKE) -C $(KDIR) \
		M=$(PWD) \
		ARCH=$(ARCH) \
		CC="$(CC)" \
		LD="$(LD)" \
		CROSS_COMPILE=$(CROSS_COMPILE) \
		CLANG_TRIPLE=$(CLANG_TRIPLE) \
		LLVM=$(LLVM) \
		EXTRA_CFLAGS="$(EXTRA_CFLAGS)" \
		modules
	@echo "=== 产物 ==="
	@ls -la mykc_core.ko 2>/dev/null || echo "!! 编译失败"

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f *.ko *.o *.mod *.mod.c modules.order Module.symvers

# ---- 打包成自释放脚本（学 paradise，base64 内嵌 + insmod）----
pack: all
	@printf '#!/bin/sh\n# mykc - self extract + insmod\nset -e\nTMP=$$(mktemp)\ntrap "rm -f \\"$$TMP\\"" EXIT INT TERM\nbase64 -d <<'"'"'__MYKC_KO__'"'"' > "$$TMP"\n' > mykc.ko.sh
	@base64 mykc_core.ko >> mykc.ko.sh
	@printf '__MYKC_KO__\ninsmod "$$TMP"\nif [ $$? -eq 0 ]; then echo "mykc loaded OK"; else echo "mykc load FAILED"; exit 1; fi\n' >> mykc.ko.sh
	@chmod 755 mykc.ko.sh
	@echo "完成: mykc.ko.sh ($$(wc -c < mykc.ko.sh) bytes)"

# ---- 检查内核版本是否匹配 ----
check:
	@echo "本机内核: 6.6.118-android15-8-gc44b714366cc-abogki519650608-4k"
	@echo "需要匹配: KMI android15-8 / 6.6.x GKI"
	@grep -E 'CONFIG_MODULE_SIG_FORCE|CONFIG_CFI_CLANG' $(KDIR)/.config 2>/dev/null || echo "(未找到 .config)"

.PHONY: all clean pack check