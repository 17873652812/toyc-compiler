 #pragma once

  #include "defs.h"
  #include <memory>
  #include <vector>

  namespace toyc {

  struct ASTNode {
      virtual ~ASTNode() = default;  // 基类：所有 AST 节点继承它
  };

  // 数字表达式：42, 0, 100
  struct NumberExpr : ASTNode {
      int value;
      explicit NumberExpr(int v) : value(v) {}
  };

  // return 语句
  struct ReturnStmt : ASTNode {
      std::unique_ptr<ASTNode> expr;  // return 后面的表达式
      explicit ReturnStmt(std::unique_ptr<ASTNode> e)
          : expr(std::move(e)) {}
  };

  // 函数定义
  struct FuncDef : ASTNode {
      std::string name;                     // 函数名
      std::unique_ptr<ASTNode> body;        // 函数体
      FuncDef(std::string n, std::unique_ptr<ASTNode> b)
          : name(std::move(n)), body(std::move(b)) {}
  };

  // 编译单元（整个程序）
  struct CompUnit : ASTNode {
      std::vector<std::unique_ptr<FuncDef>> funcs;  // 若干函数
  };

  }  // namespace toyc
