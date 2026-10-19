#!/bin/sh
# loadtest.sh — 在设备上逐级测试 mp.ko 装载
#
# 用法（root / KernelSU 环境）:
#   sh loadtest.sh
#   sh loadtest.sh 0        # 只测 STEP 0
#
# 原理：每级 insmod 后等 3 秒，检查模块是否还在（/proc/modules 或 /proc/devices）。
#       如果中途重启了，脚本会中断——重启本身就说明该 STEP 崩了。
#
# ★ 重要：STEP 0 崩溃 = 编译链问题，改代码没用。
#         STEP 0 通过  = 编译链 OK，问题在具体代码，继续往上加 STEP。

DIR=$(dirname "$0")
cd "$DIR" || exit 1

START=${1:-0}

teardown() {
    # 尽量卸载，避免残留影响下一级
    for m in mp; do
        rmmod $m 2>/dev/null
    done
}

check_alive() {
    # 三种方式任一能证明模块还在
    if grep -q '^mp ' /proc/modules 2>/dev/null; then echo "ALIVE(/proc/modules)"; return 0; fi
    if [ -d /sys/module/mp ] 2>/dev/null; then echo "ALIVE(/sys/module/mp)"; return 0; fi
    if dmesg 2>/dev/null | tail -20 | grep -q 'mp: init done'; then echo "ALIVE(dmesg init done)"; return 0; fi
    echo "NOT-FOUND"
    return 1
}

for STEP in $(seq $START 5); do
    F="mp_s${STEP}.ko"
    if [ ! -f "$F" ]; then
        echo "!! 缺少 $F，跳过"
        continue
    fi

    echo "=========================================="
    echo ">>> 测试 STEP=$STEP  ($F)"
    echo "=========================================="

    teardown
    dmesg -C 2>/dev/null

    OUT=$(insmod "$F" 2>&1)
    RC=$?

    if [ $RC -ne 0 ]; then
        echo "*** insmod 失败 RC=$RC"
        echo "    $OUT"
        echo "    -> 该 STEP 装载被内核拒绝（不是 panic，是正常拒绝）"
        continue
    fi

    sleep 3
    check_alive

    echo "--- dmesg (mp 相关) ---"
    dmesg 2>/dev/null | grep -E 'mp: ' | tail -20

    teardown
    echo ""
done

echo "=== 测试结束 ==="
echo "判定："
echo "  STEP0 通过 -> 编译链正常，问题在 mykc 具体代码"
echo "  STEP0 崩溃 -> 编译链/CRC/工具链问题，代码层面无解"