#!/bin/bash
# 全量本地验证：每个 perf/*.tc
#   - gcc 原生运行 → 基准返回值
#   - 编译器非 -opt → riscv_sim → 返回值
#   - 编译器 -opt   → riscv_sim → 返回值
# 三者一致 = PASS。任何不一致或崩溃 = FAIL。
cd "$(dirname "$0")/.." || exit 1
COMPILER=./compiler.exe
SIM=./perf/riscv_sim.exe
NATDIR=$(mktemp -d 2>/dev/null || echo /tmp/toyc_native)
mkdir -p "$NATDIR"

pass=0; fail=0; skip=0
for tc in perf/*.tc; do
    name=$(basename "$tc" .tc)
    # 跳过非测试文件
    case "$name" in riscv_sim|mini|readtest*) continue;; esac

    # gcc 原生基准（强制按 C 编译；exe 放 /tmp 避开 Device Guard）
    if ! gcc -x c -O0 -w -o "$NATDIR/$name.exe" "$tc" 2>"$NATDIR/$name.gcc.err"; then
        echo "SKIP  $name  (gcc 编译失败)"
        skip=$((skip+1)); continue
    fi
    "$NATDIR/$name.exe" >/dev/null 2>&1
    exp=$?

    # 非 -opt
    if ! "$COMPILER" "$tc" > "$NATDIR/$name.orig.s" 2>"$NATDIR/$name.orig.err"; then
        echo "FAIL  $name  (编译器非-opt崩溃)  gcc=$exp"
        fail=$((fail+1)); continue
    fi
    "$SIM" < "$NATDIR/$name.orig.s" >/dev/null 2>"$NATDIR/$name.sim.err"; orig=$?

    # -opt
    if ! "$COMPILER" -opt "$tc" > "$NATDIR/$name.opt.s" 2>"$NATDIR/$name.opt.err"; then
        echo "FAIL  $name  (编译器-opt崩溃)  gcc=$exp"
        fail=$((fail+1)); continue
    fi
    "$SIM" < "$NATDIR/$name.opt.s" >/dev/null 2>"$NATDIR/$name.sim2.err"; opt=$?

    if [ "$exp" = "$orig" ] && [ "$exp" = "$opt" ]; then
        pass=$((pass+1))
        echo "PASS  $name  gcc=$exp orig=$orig opt=$opt"
    else
        fail=$((fail+1))
        echo "FAIL  $name  gcc=$exp orig=$orig opt=$opt"
    fi
done
echo "=============================="
echo "PASS=$pass  FAIL=$fail  SKIP=$skip"
