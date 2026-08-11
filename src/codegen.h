#pragma once

#include "ast.h"
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <algorithm>

namespace toyc {

class Codegen {
public:
    explicit Codegen(const CompUnit& unit, bool opt = false)
        : unit_(unit), opt_(opt) {}

    std::unordered_map<std::string, int> global_vars_;  // 全局变量名 → 值
    std::unordered_set<std::string> global_consts_;     // 全局常量名（-opt 可折叠）

    std::string generate() {
        // 先记录全局变量的值到 map（后面 IdExpr 会查）
        for (auto& g : unit_.globals) {
            if (auto* vd = dynamic_cast<const VarDecl*>(g.get()))
                global_vars_[vd->name] = eval_const(vd->init.get());
            else if (auto* cd = dynamic_cast<const ConstDecl*>(g.get())) {
                global_vars_[cd->name] = eval_const(cd->init.get());
                global_consts_.insert(cd->name);  // 常量可编译期折叠
            }
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
    // 变量位置：寄存器（s0-s11）或栈偏移
    struct VarLoc {
        bool in_reg = false;  // 在寄存器中
        int reg = 0;          // s 寄存器编号 0-11
        int off = 0;          // 栈偏移
    };

    const CompUnit& unit_;
    bool opt_;
    std::ostringstream out_;
    // 栈式作用域：每个 Block 进入时 push 一个作用域，离开时 pop
    std::vector<std::unordered_map<std::string, VarLoc>> symtab_{1};
    std::vector<std::unordered_map<std::string, int>> consts_{1};  // v1.0
    int stack_size_ = 0, label_count_ = 0, extra_stack_ = 0, next_offset_ = 0;
    int next_reg_ = 0;          // 下一个可用的 s 寄存器编号（0-11）
    int reg_temp_depth_ = 0;    // 当前用到的 t1-t6 临时寄存器深度
    int temp_base_ = 0;         // 临时区基址（-opt 时 =0，帧底）
    int var_base_ = 0;          // 溢出变量区基址
    int temp_limit_ = 256;      // 临时区大小（字节）

    void enter_scope() { symtab_.push_back({}); consts_.push_back({}); }
    void exit_scope() { symtab_.pop_back(); consts_.pop_back(); }

    // 分配变量：-opt 优先用 s 寄存器，溢出到栈
    VarLoc alloc_var(const std::string& name) {
        VarLoc loc;
        if (opt_ && next_reg_ < 12) {
            loc.in_reg = true;
            loc.reg = next_reg_++;
        } else {
            loc.off = var_base_ + next_offset_;
            next_offset_ += 4;
        }
        symtab_.back()[name] = loc;
        return loc;
    }

    // 从内到外查找变量
    VarLoc* find_var(const std::string& name) {
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
    std::vector<std::string> current_params_;       // 当前函数参数名（尾递归用）
    std::vector<VarLoc> current_param_vars_;        // 当前函数参数位置
    struct LoopLabels { int begin, end; };
    std::vector<LoopLabels> loops_;

    int new_label() { return label_count_++; }
    // RISC-V 只有 a0-a7 共 8 个参数寄存器。超出部分需要栈传参
    std::string arg_reg(int i) {
        if (i < 8) return "a" + std::to_string(i);
        return "";  // 栈传参，不返回寄存器名
    }

    // 递归统计所有变量（含嵌套 Block 内的）—— 仅非 -opt 用
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

    // ---- 预分析（-opt） ----

    // 子树里是否有函数调用
    bool contains_call(const ASTNode* n) {
        if (dynamic_cast<const CallExpr*>(n)) return true;
        if (auto* b = dynamic_cast<const BinaryExpr*>(n))
            return contains_call(b->left.get()) || contains_call(b->right.get());
        if (auto* u = dynamic_cast<const UnaryExpr*>(n))
            return contains_call(u->expr.get());
        return false;
    }

    // 表达式需要的同时内存临时槽数（保守：任意二元都算内存槽，含嵌套深度）
    // 实际生成时寄存器临时值（t1-t5）能省掉部分内存，但按最坏情况预留保证不溢出
    int max_mem_temp_expr(const ASTNode* n) {
        if (auto* b = dynamic_cast<const BinaryExpr*>(n)) {
            if (b->op == "&&" || b->op == "||")
                return std::max(max_mem_temp_expr(b->left.get()), max_mem_temp_expr(b->right.get()));
            int L = max_mem_temp_expr(b->left.get());
            int R = max_mem_temp_expr(b->right.get());
            return std::max(L, 1 + R);  // 左值占 1 槽 + 右子树深度
        }
        if (auto* u = dynamic_cast<const UnaryExpr*>(n)) return max_mem_temp_expr(u->expr.get());
        if (auto* c = dynamic_cast<const CallExpr*>(n)) {
            int m = 0;
            for (auto& a : c->args) m = std::max(m, max_mem_temp_expr(a.get()));
            return m;
        }
        return 0;
    }
    int max_mem_temp_stmt(const ASTNode* s) {
        if (auto* b = dynamic_cast<const Block*>(s)) {
            int m = 0;
            for (auto& st : b->stmts) m = std::max(m, max_mem_temp_stmt(st.get()));
            return m;
        }
        if (auto* vd = dynamic_cast<const VarDecl*>(s)) return max_mem_temp_expr(vd->init.get());
        if (auto* cd = dynamic_cast<const ConstDecl*>(s)) return max_mem_temp_expr(cd->init.get());
        if (auto* as = dynamic_cast<const AssignStmt*>(s)) return max_mem_temp_expr(as->expr.get());
        if (auto* ret = dynamic_cast<const ReturnStmt*>(s)) return ret->expr ? max_mem_temp_expr(ret->expr.get()) : 0;
        if (auto* ifs = dynamic_cast<const IfStmt*>(s)) {
            int m = max_mem_temp_expr(ifs->cond.get());
            m = std::max(m, max_mem_temp_stmt(ifs->then_stmt.get()));
            if (ifs->else_stmt) m = std::max(m, max_mem_temp_stmt(ifs->else_stmt.get()));
            return m;
        }
        if (auto* ws = dynamic_cast<const WhileStmt*>(s))
            return std::max(max_mem_temp_expr(ws->cond.get()), max_mem_temp_stmt(ws->body.get()));
        return 0;
    }

    // 尾递归：return 自调用 → 需要的参数暂存槽数
    int max_tail_args(const ASTNode* s) {
        if (auto* b = dynamic_cast<const Block*>(s)) {
            int m = 0;
            for (auto& st : b->stmts) m = std::max(m, max_tail_args(st.get()));
            return m;
        }
        if (auto* ret = dynamic_cast<const ReturnStmt*>(s)) {
            if (auto* c = dynamic_cast<const CallExpr*>(ret->expr.get()))
                if (c->func_name == current_func_ && c->args.size() == current_params_.size())
                    return (int)c->args.size();
            return 0;
        }
        if (auto* ifs = dynamic_cast<const IfStmt*>(s)) {
            int m = max_tail_args(ifs->then_stmt.get());
            if (ifs->else_stmt) m = std::max(m, max_tail_args(ifs->else_stmt.get()));
            return m;
        }
        if (auto* ws = dynamic_cast<const WhileStmt*>(s)) return max_tail_args(ws->body.get());
        return 0;
    }
    bool stmt_has_tail_call(const ASTNode* s) {
        if (auto* b = dynamic_cast<const Block*>(s)) {
            for (auto& st : b->stmts) if (stmt_has_tail_call(st.get())) return true;
            return false;
        }
        if (auto* ret = dynamic_cast<const ReturnStmt*>(s)) {
            if (auto* c = dynamic_cast<const CallExpr*>(ret->expr.get()))
                return c->func_name == current_func_;
            return false;
        }
        if (auto* ifs = dynamic_cast<const IfStmt*>(s)) {
            if (stmt_has_tail_call(ifs->then_stmt.get())) return true;
            return ifs->else_stmt && stmt_has_tail_call(ifs->else_stmt.get());
        }
        if (auto* ws = dynamic_cast<const WhileStmt*>(s)) return stmt_has_tail_call(ws->body.get());
        return false;
    }

    // ---- 编译期常量折叠（-opt） ----

    std::optional<int> try_fold(const ASTNode* e) {
        if (auto* num = dynamic_cast<const NumberExpr*>(e)) return num->value;
        if (auto* id = dynamic_cast<const IdExpr*>(e)) {
            if (int* cv = find_const(id->name)) return *cv;
            if (global_consts_.count(id->name)) return global_vars_[id->name];  // 全局常量
            return std::nullopt;
        }
        if (auto* un = dynamic_cast<const UnaryExpr*>(e)) {
            auto v = try_fold(un->expr.get());
            if (!v) return std::nullopt;
            if (un->op == "-") return -*v;
            if (un->op == "!") return *v == 0 ? 1 : 0;
            if (un->op == "+") return *v;
            return std::nullopt;
        }
        if (auto* bin = dynamic_cast<const BinaryExpr*>(e)) {
            // 短路：左为 0（&&）或非 0（||）时不求右，避免除零
            if (bin->op == "&&" || bin->op == "||") {
                auto l = try_fold(bin->left.get());
                if (!l) return std::nullopt;
                if (bin->op == "&&" && *l == 0) return 0;
                if (bin->op == "||" && *l != 0) return 1;
                auto r = try_fold(bin->right.get());
                if (!r) return std::nullopt;
                return *r != 0 ? 1 : 0;
            }
            auto l = try_fold(bin->left.get());
            auto r = try_fold(bin->right.get());
            if (!l || !r) return std::nullopt;
            const std::string& op = bin->op;
            if (op == "+") return *l + *r;
            if (op == "-") return *l - *r;
            if (op == "*") return *l * *r;
            if (op == "/") { if (*r == 0) return std::nullopt; return *l / *r; }
            if (op == "%") { if (*r == 0) return std::nullopt; return *l % *r; }
            if (op == "<") return *l < *r ? 1 : 0;
            if (op == ">") return *l > *r ? 1 : 0;
            if (op == "<=") return *l <= *r ? 1 : 0;
            if (op == ">=") return *l >= *r ? 1 : 0;
            if (op == "==") return *l == *r ? 1 : 0;
            if (op == "!=") return *l != *r ? 1 : 0;
            return std::nullopt;
        }
        return std::nullopt;
    }

    bool is_pow2(int v) { return v > 0 && (v & (v - 1)) == 0; }
    int ilog2(int v) { int k = 0; while (v > 1) { v >>= 1; k++; } return k; }

    // ---- 函数 ----

    void gen_func(const FuncDef* func) {
        if (opt_) gen_func_opt(func);
        else gen_func_orig(func);
    }

    // 非 -opt：保持原实现（功能测试走这里，零风险）
    void gen_func_orig(const FuncDef* func) {
        current_func_ = func->name;
        current_params_ = func->params;
        symtab_.clear(); symtab_.push_back({});
        consts_.clear(); consts_.push_back({});
        extra_stack_ = 0;
        next_offset_ = 0;
        next_reg_ = 0;
        var_base_ = 0;
        loops_.clear();

        // 参数 slot
        for (auto& p : func->params) alloc_var(p);
        int nvars = count_vars(func->body.get());
        int total_vars = (int)func->params.size() + nvars;
        int frame_size = total_vars * 4 + 4 + 256;  // +256 临时值缓冲区
        if (frame_size < 16) frame_size = 16;
        frame_size = (frame_size + 15) & ~15;
        stack_size_ = frame_size;
        temp_base_ = total_vars * 4 + 4;  // 变量区之后，ra之前

        out_ << ".globl " << func->name << "\n" << func->name << ":\n";
        out_ << "    addi sp, sp, -" << stack_size_ << "\n";
        out_ << "    sw ra, " << (stack_size_ - 4) << "(sp)\n";

        // 保存寄存器参数（a0-a7）+ 栈参数（从调用者栈读取）
        int np = (int)func->params.size();
        for (int i = 0; i < np; i++) {
            VarLoc loc = symtab_.back()[func->params[i]];
            int off = loc.off;
            if (i < 8)
                out_ << "    sw " << arg_reg(i) << ", " << off << "(sp)\n";
            else {
                // 栈参数：调用者存在 sp 负偏移，callee 用 stack_size_ - N 读取
                int caller_off = stack_size_ - (i - 8 + 2) * 4;  // 对应调用者的 -8, -12, ...
                out_ << "    lw t0, " << caller_off << "(sp)\n";
                out_ << "    sw t0, " << off << "(sp)\n";
            }
        }

        // 函数体直接遍历，不通过 gen_block（避免重复 enter_scope）
        for (auto& s : func->body->stmts) gen_stmt(s.get());

        out_ << ".L" << func->name << "_exit:\n";
        out_ << "    lw ra, " << (stack_size_ - 4) << "(sp)\n";
        out_ << "    addi sp, sp, " << stack_size_ << "\n";
        out_ << "    ret\n\n";
    }

    // -opt：寄存器分配 + 帧重构 + 尾递归
    void gen_func_opt(const FuncDef* func) {
        current_func_ = func->name;
        current_params_ = func->params;
        symtab_.clear(); symtab_.push_back({});
        consts_.clear(); consts_.push_back({});
        extra_stack_ = 0;
        next_offset_ = 0;
        next_reg_ = 0;
        reg_temp_depth_ = 0;
        loops_.clear();

        // 预分析：临时区大小
        int np = (int)func->params.size();
        int nstack = std::max(0, np - 8);   // 栈参数个数
        int mmt = 0, max_tail = 0;
        for (auto& s : func->body->stmts) {
            mmt = std::max(mmt, max_mem_temp_stmt(s.get()));
            max_tail = std::max(max_tail, max_tail_args(s.get()));
        }
        temp_limit_ = 4 * (mmt + 8) + 4 * max_tail + 4 * nstack + 16;
        temp_base_ = 0;              // 临时区在帧底
        var_base_ = temp_limit_;     // 溢出变量在临时区之上

        // 分配形参（优先寄存器）
        for (auto& p : func->params) {
            alloc_var(p);
            current_param_vars_.push_back(symtab_.back()[p]);
        }

        // 先生成函数体到缓冲区，统计实际用到的寄存器/变量数
        std::ostringstream body_buf;
        out_.swap(body_buf);
        for (auto& s : func->body->stmts) {
            gen_stmt(s.get());
            if (is_terminator(s.get())) break;  // 死代码：return/break/continue 后跳过
        }
        std::string body_str = out_.str();
        out_.swap(body_buf);

        // 计算帧大小
        int n_spilled = next_offset_ / 4;
        int n_saved = next_reg_;
        int frame = temp_limit_ + n_spilled * 4 + n_saved * 4 + 4;
        if (np > 8) frame = std::max(frame, (np - 7) * 4);  // 栈参数读取区
        if (frame < 16) frame = 16;
        frame = (frame + 15) & ~15;
        stack_size_ = frame;
        int save_base = temp_limit_ + n_spilled * 4;  // s 寄存器保存区

        // prologue
        out_ << ".globl " << func->name << "\n" << func->name << ":\n";
        out_ << "    addi sp, sp, -" << frame << "\n";
        out_ << "    sw ra, " << (frame - 4) << "(sp)\n";

        // 读入栈参数（i>=8）暂存到临时区顶部
        for (int i = 8; i < np; i++) {
            int caller_off = frame - (i - 8 + 2) * 4;
            int stage_off = temp_limit_ - (nstack - (i - 8)) * 4;
            out_ << "    lw t0, " << caller_off << "(sp)\n";
            out_ << "    sw t0, " << stage_off << "(sp)\n";
        }
        // 保存用到的 s 寄存器（callee-saved，调用者值保留）
        for (int k = 0; k < n_saved; k++)
            out_ << "    sw s" << k << ", " << (save_base + k * 4) << "(sp)\n";
        // 绑定 a0-a7 参数
        for (int i = 0; i < np && i < 8; i++) {
            VarLoc loc = current_param_vars_[i];
            if (loc.in_reg) out_ << "    mv s" << loc.reg << ", a" << i << "\n";
            else out_ << "    sw a" << i << ", " << loc.off << "(sp)\n";
        }
        // 绑定栈参数
        for (int i = 8; i < np; i++) {
            VarLoc loc = current_param_vars_[i];
            int stage_off = temp_limit_ - (nstack - (i - 8)) * 4;
            out_ << "    lw t0, " << stage_off << "(sp)\n";
            if (loc.in_reg) out_ << "    mv s" << loc.reg << ", t0\n";
            else out_ << "    sw t0, " << loc.off << "(sp)\n";
        }

        // 尾递归入口标签
        if (stmt_has_tail_call(func->body.get()))
            out_ << ".L" << func->name << "_start:\n";

        // 函数体
        out_ << body_str;

        // epilogue
        out_ << ".L" << func->name << "_exit:\n";
        for (int k = n_saved - 1; k >= 0; k--)
            out_ << "    lw s" << k << ", " << (save_base + k * 4) << "(sp)\n";
        out_ << "    lw ra, " << (frame - 4) << "(sp)\n";
        out_ << "    addi sp, sp, " << frame << "\n";
        out_ << "    ret\n\n";
    }

    // 尾递归调用：return 当前函数(新参数) → 绑定参数后跳回函数头
    void gen_tail_call(const CallExpr* call) {
        int n = (int)call->args.size();
        // 先求值所有参数，暂存到临时区顶部（避开参数求值用的低区）
        for (int i = 0; i < n; i++) {
            gen_expr(call->args[i].get());
            out_ << "    sw t0, " << (temp_limit_ - (n - i) * 4) << "(sp)\n";
        }
        // 按序绑定到形参位置
        for (int i = 0; i < n; i++) {
            out_ << "    lw t0, " << (temp_limit_ - (n - i) * 4) << "(sp)\n";
            VarLoc loc = current_param_vars_[i];
            if (loc.in_reg) out_ << "    mv s" << loc.reg << ", t0\n";
            else out_ << "    sw t0, " << loc.off << "(sp)\n";
        }
        out_ << "    j .L" << current_func_ << "_start\n";
    }

    void gen_block(const Block* block) {
        enter_scope();
        for (auto& s : block->stmts) {
            gen_stmt(s.get());
            if (opt_ && is_terminator(s.get())) break;  // 死代码
        }
        exit_scope();
    }

    // 该语句是否无条件结束当前块（return/break/continue）
    bool is_terminator(const ASTNode* s) {
        return dynamic_cast<const ReturnStmt*>(s)
            || dynamic_cast<const BreakStmt*>(s)
            || dynamic_cast<const ContinueStmt*>(s);
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
            VarLoc loc = alloc_var(vd->name);
            if (loc.in_reg) out_ << "    mv s" << loc.reg << ", t0\n";
            else out_ << "    sw t0, " << loc.off << "(sp)\n";
            return;
        }
        if (auto* as = dynamic_cast<const AssignStmt*>(stmt)) {
            VarLoc* loc = find_var(as->name);
            if (loc) {
                gen_expr(as->expr.get());
                if (loc->in_reg) out_ << "    mv s" << loc->reg << ", t0\n";
                else out_ << "    sw t0, " << loc->off << "(sp)\n";
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
            if (ret->expr) {
                // 尾递归：return 自调用
                if (opt_) {
                    auto* call = dynamic_cast<const CallExpr*>(ret->expr.get());
                    if (call && call->func_name == current_func_
                        && call->args.size() == current_params_.size()) {
                        gen_tail_call(call);
                        return;
                    }
                }
                gen_expr(ret->expr.get());
                out_ << "    mv a0, t0\n";
            }
            out_ << "    j .L" << current_func_ << "_exit\n";
            return;
        }
        if (auto* call = dynamic_cast<const CallExpr*>(stmt)) {
            gen_call(call);
            return;
        }
        if (auto* ifs = dynamic_cast<const IfStmt*>(stmt)) {
            // 常数条件：直接选分支（-opt）
            if (opt_) {
                if (auto v = try_fold(ifs->cond.get())) {
                    if (*v) gen_stmt(ifs->then_stmt.get());
                    else if (ifs->else_stmt) gen_stmt(ifs->else_stmt.get());
                    return;
                }
            }
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
            // while(0)：永不执行，整段删除（-opt）
            if (opt_) {
                if (auto v = try_fold(ws->cond.get())) {
                    if (*v == 0) return;
                }
            }
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
        // 常量折叠：整棵子树可编译期求值 → 直接 li（-opt）
        if (opt_) {
            if (auto v = try_fold(expr)) {
                out_ << "    li t0, " << *v << "\n";
                return;
            }
        }
        if (auto* id = dynamic_cast<const IdExpr*>(expr)) {
            // 查找顺序：常量 > 局部变量 > 全局变量
            int* cv = find_const(id->name);
            if (cv) { out_ << "    li t0, " << *cv << "\n"; return; }
            VarLoc* loc = find_var(id->name);
            if (loc) {
                if (loc->in_reg) out_ << "    mv t0, s" << loc->reg << "\n";
                else out_ << "    lw t0, " << loc->off << "(sp)\n";
                return;
            }
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
            // 代数化简（-opt）
            if (opt_ && simplify_binary(bin)) return;
            // 普通二元运算
            gen_expr(bin->left.get());
            // 左值暂存：深度<5 且右子树无调用时用寄存器 t1-t5；
            // 否则用内存（弹回 t6，t6 专做临时，不参与寄存器临时值，绝不覆盖外层值）
            if (opt_ && reg_temp_depth_ < 5 && !contains_call(bin->right.get())) {
                reg_temp_depth_++;
                out_ << "    mv t" << reg_temp_depth_ << ", t0\n";
                gen_expr(bin->right.get());
                gen_bin_op(bin->op, "t" + std::to_string(reg_temp_depth_));
                reg_temp_depth_--;
            } else {
                extra_stack_ += 4;
                out_ << "    sw t0, " << (temp_base_ + extra_stack_) << "(sp)\n";
                gen_expr(bin->right.get());
                out_ << "    lw t6, " << (temp_base_ + extra_stack_) << "(sp)\n";
                extra_stack_ -= 4;
                gen_bin_op(bin->op, "t6");
            }
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

    // 代数化简：x+0, x-0, x*1, x*0, x*2^k, x/1, x%1
    bool simplify_binary(const BinaryExpr* bin) {
        const std::string& op = bin->op;
        auto lc = try_fold(bin->left.get());
        auto rc = try_fold(bin->right.get());
        if (op == "+" && rc && *rc == 0) { gen_expr(bin->left.get()); return true; }
        if (op == "+" && lc && *lc == 0) { gen_expr(bin->right.get()); return true; }
        if (op == "-" && rc && *rc == 0) { gen_expr(bin->left.get()); return true; }
        if (op == "*" && rc && *rc == 1) { gen_expr(bin->left.get()); return true; }
        if (op == "*" && lc && *lc == 1) { gen_expr(bin->right.get()); return true; }
        if (op == "/" && rc && *rc == 1) { gen_expr(bin->left.get()); return true; }
        if (op == "*" && rc && *rc == 0) {
            if (contains_call(bin->left.get())) gen_expr(bin->left.get());  // 有副作用先求值
            out_ << "    li t0, 0\n";
            return true;
        }
        if (op == "*" && lc && *lc == 0) {
            if (contains_call(bin->right.get())) gen_expr(bin->right.get());
            out_ << "    li t0, 0\n";
            return true;
        }
        if (op == "%" && rc && *rc == 1) {
            if (contains_call(bin->left.get())) gen_expr(bin->left.get());
            out_ << "    li t0, 0\n";
            return true;
        }
        // x * 2^k → slli（乘法精确）
        if (op == "*" && rc && *rc > 0 && is_pow2(*rc)) {
            gen_expr(bin->left.get());
            out_ << "    slli t0, t0, " << ilog2(*rc) << "\n";
            return true;
        }
        if (op == "*" && lc && *lc > 0 && is_pow2(*lc)) {
            gen_expr(bin->right.get());
            out_ << "    slli t0, t0, " << ilog2(*lc) << "\n";
            return true;
        }
        return false;
    }

    // ---- 函数调用 ----

    void gen_call(const CallExpr* call) {
        int n = (int)call->args.size();
        // 寄存器参数 a0-a7
        for (int i = 0; i < std::min(n, 8); i++) {
            gen_expr(call->args[i].get());
            out_ << "    mv " << arg_reg(i) << ", t0\n";
        }
        // 溢出参数：存到 sp 负偏移（调用者栈下方，callee 会找到）
        for (int i = 8; i < n; i++) {
            gen_expr(call->args[i].get());
            out_ << "    sw t0, -" << ((i - 8 + 2) * 4) << "(sp)\n";  // -8, -12, ... 避开 ra 的 -4
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

    // ---- 运算 ----

    void gen_bin_op(const std::string& op, const std::string& l) {
        if (op == "+") out_ << "    add t0, " << l << ", t0\n";
        else if (op == "-") out_ << "    sub t0, " << l << ", t0\n";
        else if (op == "*") out_ << "    mul t0, " << l << ", t0\n";
        else if (op == "/") out_ << "    div t0, " << l << ", t0\n";
        else if (op == "%") out_ << "    rem t0, " << l << ", t0\n";
        else if (op == "<") out_ << "    slt t0, " << l << ", t0\n";
        else if (op == ">") out_ << "    slt t0, t0, " << l << "\n";
        else if (op == "<=") { out_ << "    slt t0, t0, " << l << "\n    xori t0, t0, 1\n"; }
        else if (op == ">=") { out_ << "    slt t0, " << l << ", t0\n    xori t0, t0, 1\n"; }
        else if (op == "==") { out_ << "    sub t0, " << l << ", t0\n    seqz t0, t0\n"; }
        else if (op == "!=") { out_ << "    sub t0, " << l << ", t0\n    snez t0, t0\n"; }
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
