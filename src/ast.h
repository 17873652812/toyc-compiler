#pragma once

#include "defs.h"
#include <memory>    // std::unique_ptr — 独占指针
#include <vector>    // std::vector — 动态数组

namespace toyc {

// ============================================================
// AST（Abstract Syntax Tree）— 抽象语法树
// ============================================================
// Parser 分析完 Token 序列后，会生成一棵树来表示程序的结构
//
// 比如 int main() { return 42; } 的树：
//
//   CompUnit（整个程序）
//     └── FuncDef（函数定义，name="main"）
//           └── ReturnStmt（return 语句）
//                 └── NumberExpr（数字 42，value=42）
//
// 每种节点用一个 struct 表示，都继承自 ASTNode

// ---- ASTNode：所有 AST 节点的基类 ----
// virtual ~ASTNode() = default 是必需的：
//   它告诉 C++：删除子类对象时，要正确地销毁它
//   不用深究，每个基类都写这一行就行
struct ASTNode {
    virtual ~ASTNode() = default;
};

// ---- NumberExpr：数字表达式 ----
// 表示源码中的一个数字，比如 42、0、100
// 使用例：auto num = make_unique<NumberExpr>(42);
//              num->value  → 42
struct NumberExpr : ASTNode {       // : ASTNode 表示"继承自 ASTNode"
    int value;                       // 这个数字是多少
    explicit NumberExpr(int v) : value(v) {}
};

// ---- ReturnStmt：return 语句 ----
// 表示 return 某物;
// expr 指向 return 后面的表达式（指针，指向另一个 ASTNode）
//   比如 return 42; → expr 指向 NumberExpr(42)
//      return x;   → expr 指向 IdExpr("x")（后面会加）
//
// unique_ptr = 独占指针，意思是"expr 这个地方归我管，别人不能管"
//   你就理解成：expr 是一根线，连着 return 后面的表达式节点
struct ReturnStmt : ASTNode {
    std::unique_ptr<ASTNode> expr;   // return 后面的表达式
    explicit ReturnStmt(std::unique_ptr<ASTNode> e)
        : expr(std::move(e)) {}      // std::move(e)：把指针"移动"进来
};

// ---- FuncDef：函数定义 ----
// 表示一个函数的完整定义
// 比如 int main() { return 42; }
//   name = "main"
//   body = 指向 ReturnStmt(NumberExpr(42))
//
// 目前 v0.1 没有参数列表，v0.5 再加
struct FuncDef : ASTNode {
    std::string name;                    // 函数名，比如 "main"
    std::unique_ptr<ASTNode> body;       // 函数体（目前是一条 return 语句）
    FuncDef(std::string n, std::unique_ptr<ASTNode> b)
        : name(std::move(n)), body(std::move(b)) {}
};

// ---- CompUnit：编译单元（根节点） ----
// 代表整个程序文件
// funcs 是一个数组，装着程序中所有的函数定义
//   目前 v0.1 只有一个函数（main），后面 v0.5 会有多个
struct CompUnit : ASTNode {
    std::vector<std::unique_ptr<FuncDef>> funcs;  // 函数定义列表
};

}  // namespace toyc
