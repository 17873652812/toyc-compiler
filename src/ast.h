#pragma once

#include "defs.h"
#include <memory>
#include <vector>

namespace toyc {

// ASTNode：所有语法树节点的基类
struct ASTNode {
    virtual ~ASTNode() = default;
};

// ---- 表达式 ----

// 数字：42, 0, 100
struct NumberExpr : ASTNode {
    int value;
    explicit NumberExpr(int v) : value(v) {}
};

// 变量引用：x, y, main
struct IdExpr : ASTNode {
    std::string name;
    explicit IdExpr(std::string n) : name(std::move(n)) {}
};

// 二元运算：a + b, x * 2, y - 1
struct BinaryExpr : ASTNode {
    std::unique_ptr<ASTNode> left;
    std::string op;   // "+" "-" "*" "/" "%"
    std::unique_ptr<ASTNode> right;
    BinaryExpr(std::unique_ptr<ASTNode> l, std::string o,
               std::unique_ptr<ASTNode> r)
        : left(std::move(l)), op(std::move(o)), right(std::move(r)) {}
};

// 一元运算：-a, !x, +3
struct UnaryExpr : ASTNode {
    std::string op;   // "-" "!" "+"
    std::unique_ptr<ASTNode> expr;
    UnaryExpr(std::string o, std::unique_ptr<ASTNode> e)
        : op(std::move(o)), expr(std::move(e)) {}
};

// ---- 语句 ----

// Block：花括号包着的多条语句 { ... }
struct Block : ASTNode {
    std::vector<std::unique_ptr<ASTNode>> stmts;
};

// return 语句：return expr;
struct ReturnStmt : ASTNode {
    std::unique_ptr<ASTNode> expr;
    explicit ReturnStmt(std::unique_ptr<ASTNode> e)
        : expr(std::move(e)) {}
};

// 变量声明：int x = expr;
struct VarDecl : ASTNode {
    std::string name;
    std::unique_ptr<ASTNode> init;   // 初始值表达式
    VarDecl(std::string n, std::unique_ptr<ASTNode> i)
        : name(std::move(n)), init(std::move(i)) {}
};

// 赋值语句：x = expr;
struct AssignStmt : ASTNode {
    std::string name;
    std::unique_ptr<ASTNode> expr;
    AssignStmt(std::string n, std::unique_ptr<ASTNode> e)
        : name(std::move(n)), expr(std::move(e)) {}
};

// ---- 顶层 ----

// 函数定义
struct FuncDef : ASTNode {
    std::string name;
    std::unique_ptr<Block> body;    // v0.2: body 变成 Block
    FuncDef(std::string n, std::unique_ptr<Block> b)
        : name(std::move(n)), body(std::move(b)) {}
};

// 程序根节点
struct CompUnit : ASTNode {
    std::vector<std::unique_ptr<FuncDef>> funcs;
};

}  // namespace toyc
