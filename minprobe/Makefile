# Makefile — MYKC 最小装载验证模块
# 目标内核: Redmi K80 Pro / miro / SM8750
#           6.6.118-android15-8-gc44b714366cc-abogki519650608-4k
#
# 用法:
#   make STEP=0            # 最安全，只 pr_info
#   make STEP=1            # + vzalloc
#   make STEP=2            # + kzalloc
#   make STEP=3            # + alloc_page
#   make STEP=4            # + misc_register
#   make STEP=5            # + register_kprobe (头号嫌疑)

obj-m += mp.o

MP_STEP ?= 0
ccflags-mp.o := -DMP_STEP=$(MP_STEP)
ccflags-y += -Wno-unused-command-line-argument

# 设备内核 CONFIG_CFI_CLANG=y 且 CFI_PERMISSIVE 未开 = 严格模式
# 必须让编译器生成 kCFI 检查代码，否则间接调用会被内核拦截
ccflags-y += -fsanitize=kcfi

ARCH          ?= arm64
CLANG_TRIPLE  ?= aarch64-linux-gnu-
CC            ?= clang
LD            ?= ld.lld
LLVM          ?= 1
KDIR          ?= $(PWD)/kernel_source_tree

all:
	@echo "=== 编译 mp.ko (STEP=$(MP_STEP)) ==="
	$(MAKE) -C $(KDIR) \
		M=$(PWD) \
		ARCH=$(ARCH) \
		CC="$(CC)" \
		LD="$(LD)" \
		LLVM=$(LLVM) \
		CLANG_TRIPLE=$(CLANG_TRIPLE) \
		modules
	@echo "=== 产物 ==="
	@ls -la mp.ko 2>/dev/null || echo "!! 编译失败"

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f *.ko *.o *.mod *.mod.c modules.order Module.symvers

# 产物自检：把关键信息打出来，便于判断是不是编译链层面的问题
verify: all
	@echo "=== vermagic ==="
	@modinfo mp.ko 2>/dev/null | grep -E 'vermagic|name' || \
		strings mp.ko | grep -E '^vermagic=' || echo "(未找到 vermagic)"
	@echo "=== kCFI 检查 (brk #0x8222 数量，>0 表示已启用) ==="
	@objdump -d mp.ko 2>/dev/null | grep -c 'brk.*0x8222' || echo 0
	@echo "=== 未定义符号 (必须是内核能提供的) ==="
	@readelf -sW mp.ko 2>/dev/null | awk '$$7=="UND" && $$8!="" {print "  "$$8}' | sort -u
	@echo "=== 需要的 CRC 版本条目数 ==="
	@readelf -SW mp.ko 2>/dev/null | grep -c '__versions' || echo 0
	@echo "=== md5 ==="
	@md5sum mp.ko 2>/dev/null

.PHONY: all clean verify