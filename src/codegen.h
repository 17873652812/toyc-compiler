#pragma once

#include "ast.h"
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace toyc {

// Codegen：代码生成器，AST → RISC-V 汇编
//
// v0.2 新增：
//   - 符号表：记录每个变量的栈偏移
//   - 栈帧：为局部变量分配栈空间
//   - 表达式求值：结果放在 t0 寄存器
class Codegen {
public:
    explicit Codegen(const CompUnit& unit) : unit_(unit) {}

    std::string generate() {
        out_ << ".text\n";
        for (const auto& func : unit_.funcs) {
            gen_func(func.get());
        }
        return out_.str();
    }

private:
    const CompUnit& unit_;
    std::ostringstream out_;

    // 符号表：变量名 → 栈偏移（相对于 sp）
    std::unordered_map<std::string, int> symtab_;
    int stack_size_ = 0;   // 当前函数总共用了多少栈空间

    // 统计 Block 里有多少个 VarDecl
    int count_vars(const Block* block) {
        int n = 0;
        for (const auto& stmt : block->stmts) {
            if (dynamic_cast<const VarDecl*>(stmt.get()))
                n++;
        }
        return n;
    }

    // ---- 函数 ----

    void gen_func(const FuncDef* func) {
        // 统计变量数，算栈大小（对齐到 16 字节）
        int nvars = count_vars(func->body.get());
        stack_size_ = nvars * 4;
        if (stack_size_ > 0)
            stack_size_ = (stack_size_ + 15) & ~15;  // 16 字节对齐

        out_ << ".globl " << func->name << "\n";
        out_ << func->name << ":\n";

        // 序言：分配栈空间
        if (stack_size_ > 0)
            out_ << "    addi sp, sp, -" << stack_size_ << "\n";

        // 生成函数体
        gen_block(func->body.get());

        // 尾声：回收栈空间
        if (stack_size_ > 0)
            out_ << "    addi sp, sp, " << stack_size_ << "\n";

        out_ << "\n";
    }

    // ---- Block ----

    void gen_block(const Block* block) {
        for (const auto& stmt : block->stmts) {
            gen_stmt(stmt.get());
        }
    }

    // ---- 语句 ----

    void gen_stmt(const ASTNode* stmt) {
        // VarDecl：int x = expr;
        if (auto* vd = dynamic_cast<const VarDecl*>(stmt)) {
            gen_expr(vd->init.get());       // 计算初始值 → t0
            int offset = alloc_var(vd->name);  // 分配栈空间
            out_ << "    sw t0, " << offset << "(sp)\n"; // 存到栈上
            return;
        }

        // AssignStmt：x = expr;
        if (auto* as = dynamic_cast<const AssignStmt*>(stmt)) {
            gen_expr(as->expr.get());       // 计算右值 → t0
            auto it = symtab_.find(as->name);
            if (it == symtab_.end())
                throw std::runtime_error("undefined variable: " + as->name);
            out_ << "    sw t0, " << it->second << "(sp)\n"; // 存到栈上
            return;
        }

        // ReturnStmt：return expr;
        if (auto* ret = dynamic_cast<const ReturnStmt*>(stmt)) {
            gen_expr(ret->expr.get());      // 计算返回值 → t0
            out_ << "    mv a0, t0\n";      // 把结果放到 a0（返回值寄存器）
            out_ << "    li a7, 93\n";      // exit 系统调用号
            out_ << "    ecall\n";          // 执行系统调用
            return;
        }

        throw std::runtime_error("Codegen: unknown statement");
    }

    // ---- 表达式求值（结果放在 t0） ----

    void gen_expr(const ASTNode* expr) {
        // NumberExpr：数字 → li t0, 值
        if (auto* num = dynamic_cast<const NumberExpr*>(expr)) {
            out_ << "    li t0, " << num->value << "\n";
            return;
        }

        // IdExpr：变量 → 从栈上加载
        if (auto* id = dynamic_cast<const IdExpr*>(expr)) {
            auto it = symtab_.find(id->name);
            if (it == symtab_.end())
                throw std::runtime_error("undefined variable: " + id->name);
            out_ << "    lw t0, " << it->second << "(sp)\n";
            return;
        }

        // BinaryExpr：二元运算
        if (auto* bin = dynamic_cast<const BinaryExpr*>(expr)) {
            gen_expr(bin->left.get());      // 左操作数 → t0
            push_t0();                       // 保存 t0 到栈上
            gen_expr(bin->right.get());     // 右操作数 → t0
            pop_to_t1();                     // 恢复左操作数到 t1
            gen_bin_op(bin->op);            // t0 = t1 op t0
            return;
        }

        // UnaryExpr：一元运算
        if (auto* un = dynamic_cast<const UnaryExpr*>(expr)) {
            gen_expr(un->expr.get());       // 操作数 → t0
            gen_un_op(un->op);              // 对 t0 做一元运算
            return;
        }

        throw std::runtime_error("Codegen: unknown expression");
    }

    // ---- 栈操作辅助 ----

    // 把 t0 压入栈（临时保存）
    void push_t0() {
        out_ << "    addi sp, sp, -4\n";
        out_ << "    sw t0, 0(sp)\n";
        extra_stack_ += 4;
    }

    // 从栈弹出到 t1
    void pop_to_t1() {
        out_ << "    lw t1, 0(sp)\n";
        out_ << "    addi sp, sp, 4\n";
        extra_stack_ -= 4;
    }

    int extra_stack_ = 0;  // push/pop 用的临时栈偏移（正常情况应该为 0）

    // 分配变量的栈空间，返回偏移
    int alloc_var(const std::string& name) {
        int offset = next_offset_;
        next_offset_ += 4;
        symtab_[name] = offset;
        return offset;
    }
    int next_offset_ = 0;

    // ---- 运算 ----

    void gen_bin_op(const std::string& op) {
        if (op == "+")      out_ << "    add t0, t1, t0\n";
        else if (op == "-") out_ << "    sub t0, t1, t0\n";
        else if (op == "*") out_ << "    mul t0, t1, t0\n";
        else if (op == "/") out_ << "    div t0, t1, t0\n";
        else if (op == "%") out_ << "    rem t0, t1, t0\n";
        else throw std::runtime_error("unknown binary operator: " + op);
    }

    void gen_un_op(const std::string& op) {
        if (op == "-")      out_ << "    neg t0, t0\n";
        else if (op == "!") out_ << "    seqz t0, t0\n";
        else if (op == "+") { /* 一元 + 什么都不做 */ }
        else throw std::runtime_error("unknown unary operator: " + op);
    }
};

}  // namespace toyc
