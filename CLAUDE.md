# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

ToyC 编译器 — 把 ToyC 语言（C 的子集）编译成 RISC-V 32 位汇编。C++20 手写。

## 当前状态

零基础教学进行中。用户是编程初学者，C++、Git、终端都不太会。

已完成文件：
- `src/defs.h` — 公共定义（namespace toyc, Position 结构体）
- `src/token.h` — Token 类型枚举（10 种）
- `src/main.cpp` — 入口骨架（读 stdin，打印字符数）

## 编译运行

```bash
g++ src/main.cpp -o compiler.exe
echo 'int main() { return 42; }' | ./compiler.exe
```

## 教学原则

- 用户零基础，每个概念先解释再写代码
- 每步写完立刻编译验证
- 每次代码修改后提醒 git commit
- 终端用 Git Bash（`$` 提示符），不是 PowerShell

## 教学路线

见 `docs/教学计划_v2_最小路径.md`

## 存储与恢复

- `docs/当前进度_下次从这里继续.md` — 手动恢复用
- `C:\Users\jiure\.claude\projects\C--Users-jiure-Desktop-toyc\memory\` — Claude Code 记忆文件
