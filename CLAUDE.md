# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ToyC compiler — a hand-written compiler for a simplified C subset (ToyC), targeting **RISC-V 32-bit** assembly. Part of a university "Compiler System Practice" course, implemented in **C++20** with **CMake 4.0.3**.

## Build & Run

```bash
# Configure and build
mkdir -p build && cd build && cmake .. && make

# Compile a ToyC source file to RISC-V assembly
./compiler < input.tc > output.s

# With optimization enabled
./compiler -opt < input.tc > output.s
```

## Architecture

The compiler follows a classic 5-phase pipeline:

```
Source (.tc) → [Lexer] → Token stream → [Parser] → AST → [Semantic] → Annotated AST → [IR] → TAC → [Codegen] → RISC-V (.s)
```

Each phase is a hand-written pass — **no Lex/Yacc/ANTLR code generation is used** (though they are allowed as libraries). The project does NOT depend on Clang/LLVM.

## Key Language Rules (ToyC)

- **Types**: `int` (32-bit signed), `void` (return only)
- **Keywords**: `int void const if else while break continue return`
- **NUMBER regex**: `0|[1-9][0-9]*` (no leading zeros except `0` itself; no negative sign — negatives are handled by unary `-`)
- **ID regex**: `[_A-Za-z][_A-Za-z0-9]*`
- **Comments**: `//` (single-line), `/* */` (multi-line)
- **Entry point**: `int main()` with no parameters
- **All declarations must be initialized** (including globals)
- **`const` values must be compile-time evaluable** (only literals and already-declared consts)
- **Short-circuit evaluation** for `&&` and `||`
- **No arrays, no pointers, no I/O, no float/char, no `for` loop**

Full grammar (BNF) and semantic constraints are in `ToyC编译器任务确认文档.md`.

## Testing

```bash
# Compile a test case
./compiler < test/test1.tc > test/test1.s

# Run with RISC-V simulator (e.g. spike)
spike --isa=RV32 pk test/test1.s
echo $?   # verify exit code matches expected
```

## Directory Structure

```
toyc/
├── src/               ← 编译器源码（你写代码的地方）
├── test/              ← ToyC 测试用例 (.tc)
├── docs/              ← 文档和计划
│   ├── 编译系统实践(1).pdf
│   ├── ToyC编译器任务确认文档.md
│   ├── 教学计划.md
│   ├── 教学计划_v2_最小路径.md   ← 当前教学计划（最小路径）
│   ├── 详细实施计划书.md
│   └── 当前进度_下次从这里继续.md
├── build/             ← CMake 构建目录（自动生成）
├── .vscode/           ← VS Code 配置
└── CLAUDE.md
```

## Current Teaching Approach: Minimal Path

Start with the simplest possible compiler (v0.1) that only compiles `int main() { return 42; }`, then iteratively add features. See `docs/教学计划_v2_最小路径.md` for the full roadmap.

## Reference Files

- `docs/编译系统实践(1).pdf` — original assignment PDF from the course
- `docs/ToyC编译器任务确认文档.md` — extracted & confirmed grammar, semantic rules, and submission requirements
- `docs/教学计划.md` — original phased teaching plan (overview)
- `docs/教学计划_v2_最小路径.md` — current plan: minimal path + iterative enhancement
- `docs/详细实施计划书.md` — detailed plan with tools, git, and submission guide
