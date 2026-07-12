#pragma once

#include "ast.h"
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace toyc {

class Codegen {
public:
    explicit Codegen(const CompUnit& unit) : unit_(unit) {}

    std::string generate() {
        out_ << ".text\n";

        // _start：程序入口，调用 main，然后 exit（v0.5）
        out_ << ".globl _start\n";
        out_ << "_start:\n";
        out_ << "    call main\n";
        out_ << "    li a7, 93\n";
        out_ << "    ecall\n\n";

        for (const auto& func : unit_.funcs) {
            gen_func(func.get());
        }
        return out_.str();
    }

private:
    const CompUnit& unit_;
    std::ostringstream out_;
    std::unordered_map<std::string, int> symtab_;
    int stack_size_ = 0;
    int label_count_ = 0;
    int new_label() { return label_count_++; }
    struct LoopLabels { int begin, end; };
    std::vector<LoopLabels> loops_;
    std::string current_func_;  // 当前函数名（判断是否是 main）
    int extra_stack_ = 0;

    // 统计局部变量数（不含参数，参数用单独的 slot）
    int count_vars(const Block* block) {
        int n = 0;
        for (const auto& stmt : block->stmts) {
            if (dynamic_cast<const VarDecl*>(stmt.get())) n++;
        }
        return n;
    }

    // ---- 函数 ----

    void gen_func(const FuncDef* func) {
        current_func_ = func->name;
        symtab_.clear();
        extra_stack_ = 0;
        next_offset_ = 0;

        // 参数：从 a0, a1... 存入栈（每个参数一个 slot）
        int param_count = (int)func->params.size();
        for (int i = 0; i < param_count; i++) {
            alloc_var(func->params[i]);  // 分配栈 slot
        }
        // 局部变量
        int nvars = count_vars(func->body.get());
        // 暂时预留（在 gen_stmt VarDecl 里分配），但我们需要提前知道总数
        // 简单做法：不做预留，在 VarDecl 时动态分配
        // 重置 next_offset_，让 VarDecl 接着 params 后面分配
        // （next_offset_ 已经指向 params 之后）

        // 计算栈帧大小：变量 + ra 保存位，对齐到 16
        int total_vars = param_count + nvars;
        int frame_size = total_vars * 4 + 4;  // +4 是 ra 位
        if (frame_size < 16) frame_size = 16;        // 最小 16
        frame_size = (frame_size + 15) & ~15;         // 16 对齐
        stack_size_ = frame_size;

        out_ << ".globl " << func->name << "\n";
        out_ << func->name << ":\n";

        // 序言：分配栈空间 + 保存 ra
        out_ << "    addi sp, sp, -" << stack_size_ << "\n";
        out_ << "    sw ra, " << (stack_size_ - 4) << "(sp)\n";

        // 把参数从 a0, a1... 复制到栈上
        for (int i = 0; i < param_count; i++) {
            int off = symtab_[func->params[i]];
            std::string areg = (i == 0) ? "a0" : (i == 1) ? "a1" : "a2";
            out_ << "    sw " << areg << ", " << off << "(sp)\n";
        }

        gen_block(func->body.get());

        // 尾声（所有 return 跳到这里）
        out_ << ".L" << func->name << "_exit:\n";
        out_ << "    lw ra, " << (stack_size_ - 4) << "(sp)\n";
        out_ << "    addi sp, sp, " << stack_size_ << "\n";
        out_ << "    ret\n\n";
    }

    int next_offset_ = 0;
    int alloc_var(const std::string& name) {
        int offset = next_offset_;
        next_offset_ += 4;
        symtab_[name] = offset;
        return offset;
    }

    // ---- Block ----

    void gen_block(const Block* block) {
        for (const auto& stmt : block->stmts)
            gen_stmt(stmt.get());
    }

    // ---- 语句 ----

    void gen_stmt(const ASTNode* stmt) {
        if (auto* blk = dynamic_cast<const Block*>(stmt)) {
            gen_block(blk); return;
        }
        if (auto* vd = dynamic_cast<const VarDecl*>(stmt)) {
            gen_expr(vd->init.get());
            int offset = alloc_var(vd->name);
            out_ << "    sw t0, " << offset << "(sp)\n";
            return;
        }
        if (auto* as = dynamic_cast<const AssignStmt*>(stmt)) {
            gen_expr(as->expr.get());
            auto it = symtab_.find(as->name);
            if (it == symtab_.end())
                throw std::runtime_error("undefined: " + as->name);
            out_ << "    sw t0, " << it->second << "(sp)\n";
            return;
        }
        if (auto* ret = dynamic_cast<const ReturnStmt*>(stmt)) {
            if (ret->expr) {
                gen_expr(ret->expr.get());
                out_ << "    mv a0, t0\n";
            }
            // 跳到尾声（共用 ra 恢复 + ret）
            out_ << "    j .L" << current_func_ << "_exit\n";
            return;
        }
        if (auto* ifs = dynamic_cast<const IfStmt*>(stmt)) {
            int else_lbl = new_label(), end_lbl = new_label();
            gen_expr(ifs->cond.get());
            out_ << "    beqz t0, .L" << else_lbl << "\n";
            gen_stmt(ifs->then_stmt.get());
            out_ << "    j .L" << end_lbl << "\n";
            out_ << ".L" << else_lbl << ":\n";
            if (ifs->else_stmt) gen_stmt(ifs->else_stmt.get());
            out_ << ".L" << end_lbl << ":\n";
            return;
        }
        if (auto* ws = dynamic_cast<const WhileStmt*>(stmt)) {
            int begin_lbl = new_label(), end_lbl = new_label();
            loops_.push_back({begin_lbl, end_lbl});
            out_ << ".L" << begin_lbl << ":\n";
            gen_expr(ws->cond.get());
            out_ << "    beqz t0, .L" << end_lbl << "\n";
            gen_stmt(ws->body.get());
            out_ << "    j .L" << begin_lbl << "\n";
            out_ << ".L" << end_lbl << ":\n";
            loops_.pop_back();
            return;
        }
        // CallExpr 作为语句：f(args);（v0.5）
        if (auto* call = dynamic_cast<const CallExpr*>(stmt)) {
            int nargs = (int)call->args.size();
            for (int i = 0; i < nargs; i++) {
                gen_expr(call->args[i].get());
                std::string areg = (i == 0) ? "a0" : (i == 1) ? "a1" : "a2";
                out_ << "    mv " << areg << ", t0\n";
            }
            out_ << "    call " << call->func_name << "\n";
            return;
        }

        if (dynamic_cast<const BreakStmt*>(stmt)) {
            if (loops_.empty()) throw std::runtime_error("break outside loop");
            out_ << "    j .L" << loops_.back().end << "\n";
            return;
        }
        if (dynamic_cast<const ContinueStmt*>(stmt)) {
            if (loops_.empty()) throw std::runtime_error("continue outside loop");
            out_ << "    j .L" << loops_.back().begin << "\n";
            return;
        }
        throw std::runtime_error("Codegen: unknown statement");
    }

    // ---- 表达式 ----

    void gen_expr(const ASTNode* expr) {
        if (auto* num = dynamic_cast<const NumberExpr*>(expr)) {
            out_ << "    li t0, " << num->value << "\n";
            return;
        }
        if (auto* id = dynamic_cast<const IdExpr*>(expr)) {
            auto it = symtab_.find(id->name);
            if (it == symtab_.end())
                throw std::runtime_error("undefined: " + id->name);
            out_ << "    lw t0, " << it->second << "(sp)\n";
            return;
        }
        if (auto* bin = dynamic_cast<const BinaryExpr*>(expr)) {
            gen_expr(bin->left.get());
            push_t0();
            gen_expr(bin->right.get());
            pop_to_t1();
            gen_bin_op(bin->op);
            return;
        }
        if (auto* un = dynamic_cast<const UnaryExpr*>(expr)) {
            gen_expr(un->expr.get());
            gen_un_op(un->op);
            return;
        }
        // CallExpr：函数调用（v0.5）
        if (auto* call = dynamic_cast<const CallExpr*>(expr)) {
            // 计算参数，逐个放入 a0, a1...
            int nargs = (int)call->args.size();
            for (int i = 0; i < nargs; i++) {
                gen_expr(call->args[i].get());
                std::string areg = (i == 0) ? "a0" : (i == 1) ? "a1" : "a2";
                out_ << "    mv " << areg << ", t0\n";
            }
            out_ << "    call " << call->func_name << "\n";
            out_ << "    mv t0, a0\n";  // 返回值 → t0
            return;
        }
        throw std::runtime_error("Codegen: unknown expression");
    }

    // ---- 临时栈 push/pop（不改 sp） ----

    void push_t0() {
        extra_stack_ += 4;
        out_ << "    sw t0, -" << extra_stack_ << "(sp)\n";
    }
    void pop_to_t1() {
        out_ << "    lw t1, -" << extra_stack_ << "(sp)\n";
        extra_stack_ -= 4;
    }

    // ---- 运算 ----

    void gen_bin_op(const std::string& op) {
        if (op == "+")      out_ << "    add t0, t1, t0\n";
        else if (op == "-") out_ << "    sub t0, t1, t0\n";
        else if (op == "*") out_ << "    mul t0, t1, t0\n";
        else if (op == "/") out_ << "    div t0, t1, t0\n";
        else if (op == "%") out_ << "    rem t0, t1, t0\n";
        else if (op == "<")  out_ << "    slt t0, t1, t0\n";
        else if (op == ">")  out_ << "    slt t0, t0, t1\n";
        else if (op == "<=") { out_ << "    slt t0, t0, t1\n    xori t0, t0, 1\n"; }
        else if (op == ">=") { out_ << "    slt t0, t1, t0\n    xori t0, t0, 1\n"; }
        else if (op == "==") { out_ << "    sub t0, t1, t0\n    seqz t0, t0\n"; }
        else if (op == "!=") { out_ << "    sub t0, t1, t0\n    snez t0, t0\n"; }
        else throw std::runtime_error("unknown binop: " + op);
    }

    void gen_un_op(const std::string& op) {
        if (op == "-")      out_ << "    neg t0, t0\n";
        else if (op == "!") out_ << "    seqz t0, t0\n";
        else if (op == "+") { /* nop */ }
        else throw std::runtime_error("unknown unop: " + op);
    }
};

}  // namespace toyc
