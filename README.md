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
  - **寄存器分配**：局部变量映射到 s0-s11 寄存器，块退出回收，溢出才放栈
  - **死代码删除 DCE**：return/break/continue 后不可达语句跳过，while(0) 整段删除
  - **公共子表达式消除 CSE**：块内重复表达式缓存复用，赋值失效、分支清空
  - **常量折叠**：整棵常量表达式编译期求值
  - **代数化简**：`x+0` `x*1` `x*0` `x*2^k→slli` 等化简
  - **复制传播**：`x=y` 后读 x 直接用 y
  - **尾递归转循环**：`return f(...)` 绑定参数跳回入口，深递归不爆栈
  - **寄存器操作数**：二元运算直接操作 s 寄存器，省内存暂存
  - 多层作用域、多参数（a0-a7+栈）、全局变量、短路计算、嵌套调用均正确
