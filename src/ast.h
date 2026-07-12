#pragma once

#include "defs.h"
#include <memory>
#include <vector>

namespace toyc {

// ASTNode：所有语法树节点的基类
struct ASTNode {
    virtual ~ASTNode() = default;
};

// NumberExpr：数字表达式（比如 42）
struct NumberExpr : ASTNode {
    int value;
    explicit NumberExpr(int v) : value(v) {}
};

// ReturnStmt：return 语句（return 后面的表达式存在 expr 里）
struct ReturnStmt : ASTNode {
    std::unique_ptr<ASTNode> expr;
    explicit ReturnStmt(std::unique_ptr<ASTNode> e)
        : expr(std::move(e)) {}
};

// FuncDef：函数定义（函数名 + 函数体）
struct FuncDef : ASTNode {
    std::string name;
    std::unique_ptr<ASTNode> body;
    FuncDef(std::string n, std::unique_ptr<ASTNode> b)
        : name(std::move(n)), body(std::move(b)) {}
};

// CompUnit：整个程序的根节点（包含若干函数定义）
struct CompUnit : ASTNode {
    std::vector<std::unique_ptr<FuncDef>> funcs;
};

}  // namespace toyc
