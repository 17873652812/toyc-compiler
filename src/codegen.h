#pragma once

#include "ast.h"
#include <sstream>
#include <stdexcept>

namespace toyc {

// Codegen：代码生成器，AST 语法树 → RISC-V 汇编字符串
class Codegen {
public:
    explicit Codegen(const CompUnit& unit) : unit_(unit) {}

    // 主函数：遍历 AST，生成完整汇编
    std::string generate() {
        out_ << ".text\n";

        for (const auto& func : unit_.funcs) {
            gen_func(func.get());
        }

        return out_.str();
    }

private:
    const CompUnit& unit_;
    std::ostringstream out_;   // 字符串流，用来拼接输出

    // 生成函数定义
    void gen_func(const FuncDef* func) {
        out_ << ".globl " << func->name << "\n";
        out_ << func->name << ":\n";

        gen_stmt(func->body.get());

        out_ << "\n";
    }

    // 生成语句（目前只有 return）
    void gen_stmt(const ASTNode* stmt) {
        if (auto* ret = dynamic_cast<const ReturnStmt*>(stmt)) {
            int val = eval_expr(ret->expr.get());

            // RISC-V 退出：a0=退出码, a7=93(exit), ecall=执行
            out_ << "    li a0, " << val << "\n";
            out_ << "    li a7, 93\n";
            out_ << "    ecall\n";
        } else {
            throw std::runtime_error("Codegen: unknown statement type");
        }
    }

    // 编译期求值表达式（v0.1 只支持数字，直接返回值）
    int eval_expr(const ASTNode* expr) {
        if (auto* num = dynamic_cast<const NumberExpr*>(expr)) {
            return num->value;
        }
        throw std::runtime_error("Codegen: unknown expression type");
    }
};

}  // namespace toyc
