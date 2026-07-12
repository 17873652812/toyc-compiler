#pragma once

#include "ast.h"
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace toyc {

class Codegen {
public:
    explicit Codegen(const CompUnit& unit, bool opt = false)
        : unit_(unit), opt_(opt) {}

    std::unordered_map<std::string, int> global_vars_;  // 全局变量名 → 值

    std::string generate() {
        // 先记录全局变量的值到 map（后面 IdExpr 会查）
        for (auto& g : unit_.globals) {
            if (auto* vd = dynamic_cast<const VarDecl*>(g.get()))
                global_vars_[vd->name] = eval_const(vd->init.get());
            else if (auto* cd = dynamic_cast<const ConstDecl*>(g.get()))
                global_vars_[cd->name] = eval_const(cd->init.get());
        }

        // 全局变量放 .data 段（v1.0）
        if (!unit_.globals.empty()) {
            out_ << ".data\n";
            for (auto& g : unit_.globals) {
                gen_global(g.get());
            }
        }

        out_ << ".text\n";
        // 不定义 _start —— 评测系统的 C 运行时会提供 _start 并调用 main
        for (const auto& func : unit_.funcs)
            gen_func(func.get());
        return out_.str();
    }

    // 生成全局变量/常量
    void gen_global(const ASTNode* node) {
        if (auto* vd = dynamic_cast<const VarDecl*>(node)) {
            out_ << ".globl " << vd->name << "\n";
            out_ << vd->name << ":\n";
            out_ << "    .word " << eval_const(vd->init.get()) << "\n";
        } else if (auto* cd = dynamic_cast<const ConstDecl*>(node)) {
            out_ << ".globl " << cd->name << "\n";
            out_ << cd->name << ":\n";
            out_ << "    .word " << eval_const(cd->init.get()) << "\n";
        }
    }

private:
    const CompUnit& unit_;
    bool opt_;
    std::ostringstream out_;
    std::unordered_map<std::string, int> symtab_;   // 变量 → 栈偏移
    std::unordered_map<std::string, int> consts_;   // 常量 → 编译期值（v1.0）
    int stack_size_ = 0, label_count_ = 0, extra_stack_ = 0, next_offset_ = 0;
    std::string current_func_;
    struct LoopLabels { int begin, end; };
    std::vector<LoopLabels> loops_;

    int new_label() { return label_count_++; }

    int count_vars(const Block* block) {
        int n = 0;
        for (const auto& stmt : block->stmts)
            if (dynamic_cast<const VarDecl*>(stmt.get())) n++;
        return n;
    }

    int alloc_var(const std::string& name) {
        int off = next_offset_; next_offset_ += 4;
        symtab_[name] = off;
        return off;
    }

    // ---- 函数 ----

    void gen_func(const FuncDef* func) {
        current_func_ = func->name;
        symtab_.clear();
        consts_.clear();
        extra_stack_ = 0;
        next_offset_ = 0;

        // 参数 slot
        for (auto& p : func->params) alloc_var(p);
        int nvars = count_vars(func->body.get());
        int total_vars = (int)func->params.size() + nvars;
        int frame_size = total_vars * 4 + 4;
        if (frame_size < 16) frame_size = 16;
        frame_size = (frame_size + 15) & ~15;
        stack_size_ = frame_size;

        out_ << ".globl " << func->name << "\n" << func->name << ":\n";
        out_ << "    addi sp, sp, -" << stack_size_ << "\n";
        out_ << "    sw ra, " << (stack_size_ - 4) << "(sp)\n";

        for (int i = 0; i < (int)func->params.size(); i++) {
            int off = symtab_[func->params[i]];
            std::string r = (i == 0) ? "a0" : (i == 1) ? "a1" : "a2";
            out_ << "    sw " << r << ", " << off << "(sp)\n";
        }

        gen_block(func->body.get());

        out_ << ".L" << func->name << "_exit:\n";
        out_ << "    lw ra, " << (stack_size_ - 4) << "(sp)\n";
        out_ << "    addi sp, sp, " << stack_size_ << "\n";
        out_ << "    ret\n\n";
    }

    void gen_block(const Block* block) {
        for (auto& s : block->stmts) gen_stmt(s.get());
    }

    // ---- 语句 ----

    void gen_stmt(const ASTNode* stmt) {
        if (auto* b = dynamic_cast<const Block*>(stmt)) { gen_block(b); return; }

        // ConstDecl：编译期求值，存入常量表（v1.0）
        if (auto* cd = dynamic_cast<const ConstDecl*>(stmt)) {
            int val = eval_const(cd->init.get());
            consts_[cd->name] = val;
            return;
        }

        if (auto* vd = dynamic_cast<const VarDecl*>(stmt)) {
            gen_expr(vd->init.get());
            int off = alloc_var(vd->name);
            out_ << "    sw t0, " << off << "(sp)\n";
            return;
        }
        if (auto* as = dynamic_cast<const AssignStmt*>(stmt)) {
            auto it = symtab_.find(as->name);
            if (it == symtab_.end()) throw std::runtime_error("undefined: " + as->name);
            gen_expr(as->expr.get());
            out_ << "    sw t0, " << it->second << "(sp)\n";
            return;
        }
        if (auto* ret = dynamic_cast<const ReturnStmt*>(stmt)) {
            if (ret->expr) { gen_expr(ret->expr.get()); out_ << "    mv a0, t0\n"; }
            out_ << "    j .L" << current_func_ << "_exit\n";
            return;
        }
        if (auto* call = dynamic_cast<const CallExpr*>(stmt)) {
            gen_call(call);
            return;
        }
        if (auto* ifs = dynamic_cast<const IfStmt*>(stmt)) {
            int el = new_label(), en = new_label();
            gen_expr(ifs->cond.get());
            out_ << "    beqz t0, .L" << el << "\n";
            gen_stmt(ifs->then_stmt.get());
            out_ << "    j .L" << en << "\n";
            out_ << ".L" << el << ":\n";
            if (ifs->else_stmt) gen_stmt(ifs->else_stmt.get());
            out_ << ".L" << en << ":\n";
            return;
        }
        if (auto* ws = dynamic_cast<const WhileStmt*>(stmt)) {
            int bg = new_label(), en = new_label();
            loops_.push_back({bg, en});
            out_ << ".L" << bg << ":\n";
            gen_expr(ws->cond.get());
            out_ << "    beqz t0, .L" << en << "\n";
            gen_stmt(ws->body.get());
            out_ << "    j .L" << bg << "\n";
            out_ << ".L" << en << ":\n";
            loops_.pop_back();
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
            // 查找顺序：常量 > 局部变量 > 全局变量
            auto ci = consts_.find(id->name);
            if (ci != consts_.end()) {
                out_ << "    li t0, " << ci->second << "\n";
                return;
            }
            auto it = symtab_.find(id->name);
            if (it != symtab_.end()) {
                out_ << "    lw t0, " << it->second << "(sp)\n";
                return;
            }
            auto gi = global_vars_.find(id->name);
            if (gi != global_vars_.end()) {
                out_ << "    li t0, " << gi->second << "\n";
                return;
            }
            throw std::runtime_error("undefined: " + id->name);
            if (it == symtab_.end()) throw std::runtime_error("undefined: " + id->name);
            out_ << "    lw t0, " << it->second << "(sp)\n";
            return;
        }
        if (auto* bin = dynamic_cast<const BinaryExpr*>(expr)) {
            // 短路计算（v1.0）
            if (bin->op == "&&") { gen_short_circuit_and(bin); return; }
            if (bin->op == "||") { gen_short_circuit_or(bin); return; }
            // 普通二元运算
            gen_expr(bin->left.get());
            push_t0();
            gen_expr(bin->right.get());
            pop_to_t1();
            gen_bin_op(bin->op);
            return;
        }
        if (auto* un = dynamic_cast<const UnaryExpr*>(expr)) {
            gen_expr(un->expr.get()); gen_un_op(un->op); return;
        }
        if (auto* call = dynamic_cast<const CallExpr*>(expr)) {
            gen_call(call); return;
        }
        throw std::runtime_error("Codegen: unknown expression");
    }

    // ---- 函数调用 ----

    void gen_call(const CallExpr* call) {
        int n = (int)call->args.size();
        for (int i = 0; i < n; i++) {
            gen_expr(call->args[i].get());
            std::string r = (i == 0) ? "a0" : (i == 1) ? "a1" : "a2";
            out_ << "    mv " << r << ", t0\n";
        }
        out_ << "    call " << call->func_name << "\n";
        out_ << "    mv t0, a0\n";
    }

    // ---- 短路计算（v1.0） ----

    void gen_short_circuit_and(const BinaryExpr* bin) {
        int end_lbl = new_label(), false_lbl = new_label();
        gen_expr(bin->left.get());
        out_ << "    beqz t0, .L" << false_lbl << "\n";   // 左假 → 短路
        gen_expr(bin->right.get());
        out_ << "    beqz t0, .L" << false_lbl << "\n";   // 右假 → 短路
        out_ << "    li t0, 1\n";                           // 都为真 → 1
        out_ << "    j .L" << end_lbl << "\n";
        out_ << ".L" << false_lbl << ":\n";
        out_ << "    li t0, 0\n";
        out_ << ".L" << end_lbl << ":\n";
    }

    void gen_short_circuit_or(const BinaryExpr* bin) {
        int end_lbl = new_label(), true_lbl = new_label();
        gen_expr(bin->left.get());
        out_ << "    bnez t0, .L" << true_lbl << "\n";     // 左真 → 短路
        gen_expr(bin->right.get());
        out_ << "    bnez t0, .L" << true_lbl << "\n";     // 右真 → 短路
        out_ << "    li t0, 0\n";                           // 都为假 → 0
        out_ << "    j .L" << end_lbl << "\n";
        out_ << ".L" << true_lbl << ":\n";
        out_ << "    li t0, 1\n";
        out_ << ".L" << end_lbl << ":\n";
    }

    // ---- 编译期常量求值（v1.0） ----

    int eval_const(const ASTNode* expr) {
        if (auto* num = dynamic_cast<const NumberExpr*>(expr))
            return num->value;
        if (auto* id = dynamic_cast<const IdExpr*>(expr)) {
            auto it = consts_.find(id->name);
            if (it != consts_.end()) return it->second;
            throw std::runtime_error("const init requires compile-time value: " + id->name);
        }
        if (auto* bin = dynamic_cast<const BinaryExpr*>(expr)) {
            int l = eval_const(bin->left.get());
            int r = eval_const(bin->right.get());
            std::string op = bin->op;
            if (op == "+") return l + r;
            if (op == "-") return l - r;
            if (op == "*") return l * r;
            if (op == "/") return l / r;
            if (op == "%") return l % r;
            throw std::runtime_error("unsupported op in const: " + op);
        }
        if (auto* un = dynamic_cast<const UnaryExpr*>(expr)) {
            int v = eval_const(un->expr.get());
            if (un->op == "-") return -v;
            if (un->op == "!") return !v;
            return v;
        }
        throw std::runtime_error("non-const expression in const init");
    }

    // ---- 临时栈 ----

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
        if (op == "+") out_ << "    add t0, t1, t0\n";
        else if (op == "-") out_ << "    sub t0, t1, t0\n";
        else if (op == "*") out_ << "    mul t0, t1, t0\n";
        else if (op == "/") out_ << "    div t0, t1, t0\n";
        else if (op == "%") out_ << "    rem t0, t1, t0\n";
        else if (op == "<") out_ << "    slt t0, t1, t0\n";
        else if (op == ">") out_ << "    slt t0, t0, t1\n";
        else if (op == "<=") { out_ << "    slt t0, t0, t1\n    xori t0, t0, 1\n"; }
        else if (op == ">=") { out_ << "    slt t0, t1, t0\n    xori t0, t0, 1\n"; }
        else if (op == "==") { out_ << "    sub t0, t1, t0\n    seqz t0, t0\n"; }
        else if (op == "!=") { out_ << "    sub t0, t1, t0\n    snez t0, t0\n"; }
        else throw std::runtime_error("unknown binop: " + op);
    }

    void gen_un_op(const std::string& op) {
        if (op == "-") out_ << "    neg t0, t0\n";
        else if (op == "!") out_ << "    seqz t0, t0\n";
        else if (op == "+") { /* nop */ }
        else throw std::runtime_error("unknown unop: " + op);
    }
};

}  // namespace toyc
