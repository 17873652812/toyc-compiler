#!/bin/bash
# 全量本地验证：每个 perf/*.tc
#   - gcc 原生运行 → 基准返回值（打印，不受 Windows 负退出码 127 干扰）
#   - 编译器非 -opt → riscv_sim → 返回值
#   - 编译器 -opt   → riscv_sim → 返回值
# 三者一致 = PASS。任何不一致或崩溃 = FAIL。
cd "$(dirname "$0")/.." || exit 1
COMPILER=./compiler.exe
[ -x "$COMPILER" ] || COMPILER="C:/Users/jiure/Desktop/tcc.exe"
SIM=./perf/riscv_sim.exe
NATDIR=$(mktemp -d 2>/dev/null || echo /tmp/toyc_native)
mkdir -p "$NATDIR"

# gcc 原生：把测试的 main 改名为 test_main，包一层 main 打印返回值（避开 Windows 负退出码）
cat > "$NATDIR/wrapper.c" <<'EOF'
#include <stdio.h>
int test_main(void);
int main(void) { printf("%d\n", test_main()); return 0; }
EOF

pass=0; fail=0; skip=0
for tc in perf/*.tc; do
    name=$(basename "$tc" .tc)
    # 跳过非测试文件
    case "$name" in riscv_sim|mini|readtest*) continue;; esac

    # gcc 原生基准
    if ! gcc -x c -O0 -w -Dmain=test_main -c "$tc" -o "$NATDIR/$name.o" 2>"$NATDIR/$name.gcc.err"; then
        echo "SKIP  $name  (gcc 编译失败)"
        skip=$((skip+1)); continue
    fi
    gcc -O0 "$NATDIR/$name.o" "$NATDIR/wrapper.c" -o "$NATDIR/$name.exe" 2>/dev/null
    exp=$("$NATDIR/$name.exe" 2>/dev/null)

    # 非 -opt
    if ! "$COMPILER" "$tc" > "$NATDIR/$name.orig.s" 2>"$NATDIR/$name.orig.err"; then
        echo "FAIL  $name  (编译器非-opt崩溃)  gcc=$exp"
        fail=$((fail+1)); continue
    fi
    orig=$("$SIM" < "$NATDIR/$name.orig.s" 2>"$NATDIR/$name.sim.err")

    # -opt
    if ! "$COMPILER" -opt "$tc" > "$NATDIR/$name.opt.s" 2>"$NATDIR/$name.opt.err"; then
        echo "FAIL  $name  (编译器-opt崩溃)  gcc=$exp"
        fail=$((fail+1)); continue
    fi
    opt=$("$SIM" < "$NATDIR/$name.opt.s" 2>"$NATDIR/$name.sim2.err")

    if [ -z "$exp" ]; then
        # gcc 原生崩溃（深递归栈溢出等）→ 只看编译器两路径是否一致
        if [ "$orig" = "$opt" ]; then
            pass=$((pass+1))
            echo "PASS  $name  (gcc原生崩溃) orig=$orig opt=$opt"
        else
            fail=$((fail+1))
            echo "FAIL  $name  (gcc原生崩溃) orig=$orig opt=$opt"
        fi
    elif [ "$exp" = "$orig" ] && [ "$exp" = "$opt" ]; then
        pass=$((pass+1))
        echo "PASS  $name  gcc=$exp orig=$orig opt=$opt"
    else
        fail=$((fail+1))
        echo "FAIL  $name  gcc=$exp orig=$orig opt=$opt"
    fi
done
echo "=============================="
echo "PASS=$pass  FAIL=$fail  SKIP=$skip"
