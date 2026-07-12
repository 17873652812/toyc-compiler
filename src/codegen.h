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
    // 栈式作用域：每个 Block 进入时 push 一个作用域，离开时 pop
    std::vector<std::unordered_map<std::string, int>> symtab_{1};
    std::vector<std::unordered_map<std::string, int>> consts_{1};  // v1.0
    int stack_size_ = 0, label_count_ = 0, extra_stack_ = 0, next_offset_ = 0;

    void enter_scope() { symtab_.push_back({}); consts_.push_back({}); }
    void exit_scope() { symtab_.pop_back(); consts_.pop_back(); }

    int alloc_var(const std::string& name) {
        int off = next_offset_; next_offset_ += 4;
        symtab_.back()[name] = off;
        return off;
    }

    // 从内到外查找变量
    int* find_var(const std::string& name) {
        for (int i = (int)symtab_.size() - 1; i >= 0; i--) {
            auto it = symtab_[i].find(name);
            if (it != symtab_[i].end()) return &it->second;
        }
        return nullptr;
    }

    int* find_const(const std::string& name) {
        for (int i = (int)consts_.size() - 1; i >= 0; i--) {
            auto it = consts_[i].find(name);
            if (it != consts_[i].end()) return &it->second;
        }
        return nullptr;
    }
    std::string current_func_;
    struct LoopLabels { int begin, end; };
    std::vector<LoopLabels> loops_;

    int new_label() { return label_count_++; }
    // RISC-V 只有 a0-a7 共 8 个参数寄存器。超出部分需要栈传参
    std::string arg_reg(int i) {
        if (i < 8) return "a" + std::to_string(i);
        return "";  // 栈传参，不返回寄存器名
    }

    // 递归统计所有变量（含嵌套 Block 内的）
    int count_vars(const Block* block) {
        int n = 0;
        for (const auto& stmt : block->stmts) {
            if (dynamic_cast<const VarDecl*>(stmt.get())) n++;
            else if (auto* b = dynamic_cast<const Block*>(stmt.get()))
                n += count_vars(b);
            else if (auto* ifs = dynamic_cast<const IfStmt*>(stmt.get())) {
                n += count_vars_in_stmt(ifs->then_stmt.get());
                if (ifs->else_stmt) n += count_vars_in_stmt(ifs->else_stmt.get());
            }
            else if (auto* ws = dynamic_cast<const WhileStmt*>(stmt.get()))
                n += count_vars_in_stmt(ws->body.get());
        }
        return n;
    }
    int count_vars_in_stmt(const ASTNode* s) {
        if (auto* b = dynamic_cast<const Block*>(s)) return count_vars(b);
        if (auto* vd = dynamic_cast<const VarDecl*>(s)) return 1;
        return 0;
    }

    // ---- 函数 ----

    void gen_func(const FuncDef* func) {
        current_func_ = func->name;
        symtab_.clear(); symtab_.push_back({});
        consts_.clear(); consts_.push_back({});
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

        // 只保存前 8 个寄存器参数（a0-a7），超出部分暂不支持
        int nparams = std::min((int)func->params.size(), 8);
        for (int i = 0; i < nparams; i++) {
            int off = symtab_.back()[func->params[i]];
            out_ << "    sw " << arg_reg(i) << ", " << off << "(sp)\n";
        }

        // 函数体直接遍历，不通过 gen_block（避免重复 enter_scope）
        for (auto& s : func->body->stmts) gen_stmt(s.get());

        out_ << ".L" << func->name << "_exit:\n";
        out_ << "    lw ra, " << (stack_size_ - 4) << "(sp)\n";
        out_ << "    addi sp, sp, " << stack_size_ << "\n";
        out_ << "    ret\n\n";
    }

    void gen_block(const Block* block) {
        enter_scope();
        for (auto& s : block->stmts) gen_stmt(s.get());
        exit_scope();
    }

    // ---- 语句 ----

    void gen_stmt(const ASTNode* stmt) {
        if (auto* b = dynamic_cast<const Block*>(stmt)) { gen_block(b); return; }

        // ConstDecl：编译期求值，存入常量表（v1.0）
        if (auto* cd = dynamic_cast<const ConstDecl*>(stmt)) {
            int val = eval_const(cd->init.get());
            consts_.back()[cd->name] = val;
            return;
        }

        if (auto* vd = dynamic_cast<const VarDecl*>(stmt)) {
            gen_expr(vd->init.get());
            int off = alloc_var(vd->name);
            out_ << "    sw t0, " << off << "(sp)\n";
            return;
        }
        if (auto* as = dynamic_cast<const AssignStmt*>(stmt)) {
            int* off = find_var(as->name);
            if (off) {
                gen_expr(as->expr.get());
                out_ << "    sw t0, " << *off << "(sp)\n";
                return;
            }
            // 全局变量赋值（v1.0）
            auto gi = global_vars_.find(as->name);
            if (gi != global_vars_.end()) {
                gen_expr(as->expr.get());
                out_ << "    la t1, " << as->name << "\n";
                out_ << "    sw t0, 0(t1)\n";
                global_vars_[as->name] = 0;  // 值不再可静态确定
                return;
            }
            throw std::runtime_error("undefined: " + as->name);
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
        // 裸表达式语句：x;  5;  (a+b); — 求值后丢弃结果
        if (dynamic_cast<const NumberExpr*>(stmt)
            || dynamic_cast<const IdExpr*>(stmt)
            || dynamic_cast<const BinaryExpr*>(stmt)
            || dynamic_cast<const UnaryExpr*>(stmt)
            || dynamic_cast<const CallExpr*>(stmt)) {
            gen_expr(stmt);  // 求值，结果在 t0，不需要用
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
            int* cv = find_const(id->name);
            if (cv) { out_ << "    li t0, " << *cv << "\n"; return; }
            int* off = find_var(id->name);
            if (off) { out_ << "    lw t0, " << *off << "(sp)\n"; return; }
            auto gi = global_vars_.find(id->name);
            if (gi != global_vars_.end()) {
                // 从全局标签加载（支持运行时修改）
                out_ << "    la t0, " << id->name << "\n";
                out_ << "    lw t0, 0(t0)\n";
                return;
            }
            throw std::runtime_error("undefined: " + id->name);
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
        int n = std::min((int)call->args.size(), 8);  // 最多 8 个寄存器参数
        for (int i = 0; i < n; i++) {
            gen_expr(call->args[i].get());
            out_ << "    mv " << arg_reg(i) << ", t0\n";
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
            int* cv = find_const(id->name);
            if (cv) return *cv;
            auto gi = global_vars_.find(id->name);
            if (gi != global_vars_.end()) return gi->second;
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
