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
源码 → Lexer → Parser → AST → Codegen → RISC-V 汇编
```

- `src/lexer.h` `src/parser.h` `src/ast.h` — 词法/语法分析，构建 AST
- `src/codegen.h` — 代码生成（AST 直接生成汇编，单遍）
  - 非 `-opt`：`gen_func_orig`，变量全部放栈，正确性优先
  - `-opt`：`gen_func_opt`，开启优化：
    - **寄存器分配**：局部变量映射到 s0-s11 寄存器，溢出才放栈
    - **死代码删除 DCE**：return/break/continue 后不可达语句跳过
    - **公共子表达式消除 CSE**：块内重复表达式（变量/常量操作数）缓存复用，变量赋值时失效
    - **常量折叠**：编译期可算的表达式直接求值
    - **复制传播**：`x = y` 后读 x 直接用 y
    - **代数化简**：`x+0` `x*1` `x*2^k` 等化简
    - **尾递归转循环**：`return f(...)` 跳回函数头，深递归不爆栈
