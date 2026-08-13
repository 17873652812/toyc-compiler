# ToyC 编译器

把 ToyC 语言（C 的子集）编译成 RISC-V 32 位汇编。C++20 手写。

## 构建

```bash
g++ -std=c++20 src/main.cpp -o compiler
```

或使用 CMake：

```bash
cmake -S . -B build
cmake --build build
```

## 使用

```bash
./compiler < input.tc > output.s      # 不优化（功能测试）
./compiler -opt < input.tc > output.s # 开启优化（性能测试）
```

输出为 RISC-V RV32I 汇编，程序 `main` 的返回值通过 `a0` 返回。

## 架构

```
AST ──► IR（三地址码，虚拟寄存器）──► 优化（-opt）──► 寄存器分配 ──► RISC-V 汇编
```

- `src/lexer.h` `src/parser.h` `src/ast.h` — 词法/语法分析，构建 AST
- `src/codegen.h` — 非 `-opt` 后端（AST 直接生成汇编）
- `src/ir_backend.h` — `-opt` 后端：AST → IR → 优化 → 寄存器分配 → 汇编
  - 优化（基本块内，迭代到不动点）：常量折叠与传播、复制传播、公共子表达式消除、死代码删除、代数化简与立即数融合、尾递归转循环
  - 寄存器分配：活跃分析（CFG 反向数据流）+ 线性扫描
