#pragma once

#include "ast.h"
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

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
    // 变量位置：s 寄存器（s0-s11）或栈偏移
    struct VarLoc {
        bool in_reg = false;  // 在寄存器中
        int reg = 0;          // s 寄存器编号 0-11
        int off = 0;          // 栈偏移（溢出变量）
    };
    // 栈式作用域：每个 Block 进入时 push 一个作用域，离开时 pop
    std::vector<std::unordered_map<std::string, VarLoc>> symtab_{1};
    std::vector<std::unordered_map<std::string, int>> consts_{1};  // v1.0
    int stack_size_ = 0, label_count_ = 0, extra_stack_ = 0, next_offset_ = 0;
    int next_reg_ = 0;        // 下一个可用的 s 寄存器编号（0-11）
    int max_reg_used_ = 0;    // 本函数实际用到的最大 s 寄存器数（保存/恢复用）
    std::vector<int> scope_reg_base_;   // 每个作用域进入时的寄存器水位（退出时回收）

    // ---- CSE（公共子表达式消除） ----
    static const int CSE_SLOTS = 8;   // CSE 结果缓存槽位数
    int cse_base_ = 0;                 // CSE 存储区基址（帧内偏移）
    int cse_count_ = 0;                // 已用槽位数
    struct CseEntry { std::string l, r; int slot; };  // 两操作数标记 + 槽位
    std::unordered_map<std::string, CseEntry> cse_map_;  // key(op,l,r) → 条目

    // ---- 复制传播 ----
    std::unordered_map<std::string, std::string> copy_tab_;  // 变量 → 其来源变量（x=y → x→y）

    // 进入/退出作用域：记录寄存器水位，退出时回收块内变量占用的寄存器
    void enter_scope() {
        symtab_.push_back({}); consts_.push_back({});
        scope_reg_base_.push_back(next_reg_);
    }
    void exit_scope() {
        symtab_.pop_back(); consts_.pop_back();
        next_reg_ = scope_reg_base_.back();   // 回收块内寄存器
        scope_reg_base_.pop_back();
        copy_tab_.clear();   // 块内变量销毁，复制源可能失效，清空副本表
    }

    // 分配变量位置：优先 s 寄存器，超 12 个溢出到栈
    VarLoc alloc_var(const std::string& name) {
        VarLoc loc;
        if (next_reg_ < 12) {
            loc.in_reg = true;
            loc.reg = next_reg_++;
            if (next_reg_ > max_reg_used_) max_reg_used_ = next_reg_;
        } else {
            loc.off = next_offset_; next_offset_ += 4;
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
        if (dynamic_cast<const VarDecl*>(s)) return 1;
        return 0;
    }

    // ---- 函数 ----

    void gen_func(const FuncDef* func) {
        current_func_ = func->name;
        symtab_.clear(); symtab_.push_back({});
        consts_.clear(); consts_.push_back({});
        extra_stack_ = 0;
        next_offset_ = 0;
        next_reg_ = 0;
        max_reg_used_ = 0;
        scope_reg_base_.clear();

        // 参数分配位置（优先寄存器）
        for (auto& p : func->params) alloc_var(p);
        int nvars = count_vars(func->body.get());
        int total_vars = (int)func->params.size() + nvars;
        int n_reg = std::min(total_vars, 12);   // 最多 12 个 s 寄存器

        // 帧布局：[溢出变量区][s寄存器保存区][临时区][CSE区][ra]
        int sreg_save_base = total_vars * 4;    // 溢出变量区之后（安全边界）
        temp_base_ = total_vars * 4 + n_reg * 4 + 4;  // 临时区在保存区之后
        cse_base_ = temp_base_ + 256;           // CSE 区在临时区(256字节)之后
        int frame_size = cse_base_ + CSE_SLOTS * 4 + 4;  // CSE + ra
        if (frame_size < 16) frame_size = 16;
        frame_size = (frame_size + 15) & ~15;
        stack_size_ = frame_size;
        cse_count_ = 0; cse_map_.clear();

        out_ << ".globl " << func->name << "\n" << func->name << ":\n";
        out_ << "    addi sp, sp, -" << stack_size_ << "\n";
        out_ << "    sw ra, " << (stack_size_ - 4) << "(sp)\n";

        // 保存本函数用到的 s 寄存器（callee-saved，调用者值保留）
        for (int k = 0; k < n_reg; k++)
            out_ << "    sw s" << k << ", " << (sreg_save_base + k * 4) << "(sp)\n";

        // 保存寄存器参数（a0-a7）+ 栈参数（从调用者栈读取）
        int np = (int)func->params.size();
        for (int i = 0; i < np; i++) {
            VarLoc loc = symtab_.back()[func->params[i]];
            if (i < 8) {
                if (loc.in_reg) out_ << "    mv s" << loc.reg << ", " << arg_reg(i) << "\n";
                else out_ << "    sw " << arg_reg(i) << ", " << loc.off << "(sp)\n";
            } else {
                // 栈参数：调用者存在 sp 负偏移，callee 用 stack_size_ - N 读取
                int caller_off = stack_size_ - (i - 8 + 2) * 4;  // 对应调用者的 -8, -12, ...
                out_ << "    lw t0, " << caller_off << "(sp)\n";
                if (loc.in_reg) out_ << "    mv s" << loc.reg << ", t0\n";
                else out_ << "    sw t0, " << loc.off << "(sp)\n";
            }
        }

        // 函数体直接遍历，不通过 gen_block（避免重复 enter_scope）
        for (auto& s : func->body->stmts) {
            gen_stmt(s.get());
            if (is_terminator(s.get())) break;   // 死代码：return 后语句不可达，跳过
        }

        // 尾声：恢复 s 寄存器 + ra
        out_ << ".L" << func->name << "_exit:\n";
        for (int k = n_reg - 1; k >= 0; k--)
            out_ << "    lw s" << k << ", " << (sreg_save_base + k * 4) << "(sp)\n";
        out_ << "    lw ra, " << (stack_size_ - 4) << "(sp)\n";
        out_ << "    addi sp, sp, " << stack_size_ << "\n";
        out_ << "    ret\n\n";
    }

    // 该语句是否无条件结束当前块（return/break/continue）——用于死代码删除
    bool is_terminator(const ASTNode* s) {
        return dynamic_cast<const ReturnStmt*>(s)
            || dynamic_cast<const BreakStmt*>(s)
            || dynamic_cast<const ContinueStmt*>(s);
    }

    void gen_block(const Block* block) {
        enter_scope();
        for (auto& s : block->stmts) {
            gen_stmt(s.get());
            if (is_terminator(s.get())) break;   // 死代码：后续语句不可达，跳过
        }
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
            invalidate_copy(vd->name);
            cse_invalidate(vd->name);   // 新变量可能遮蔽同名旧变量，CSE 失效
            // 复制传播：int x = y → x 读 y 的位置
            if (auto* sid = dynamic_cast<const IdExpr*>(vd->init.get()))
                if (find_var(sid->name) && !global_vars_.count(sid->name))
                    copy_tab_[vd->name] = copy_source(sid->name);
            VarLoc loc = alloc_var(vd->name);
            if (loc.in_reg) out_ << "    mv s" << loc.reg << ", t0\n";
            else out_ << "    sw t0, " << loc.off << "(sp)\n";
            return;
        }
        if (auto* as = dynamic_cast<const AssignStmt*>(stmt)) {
            VarLoc* loc = find_var(as->name);
            if (loc) {
                gen_expr(as->expr.get());
                invalidate_copy(as->name);
                cse_invalidate(as->name);   // 变量被赋值，以它为操作数的缓存失效
                // 复制传播：x = y → x 读 y 的位置
                if (auto* sid = dynamic_cast<const IdExpr*>(as->expr.get()))
                    if (find_var(sid->name) && !global_vars_.count(sid->name))
                        copy_tab_[as->name] = copy_source(sid->name);
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
            if (ret->expr) { gen_expr(ret->expr.get()); out_ << "    mv a0, t0\n"; }
            out_ << "    j .L" << current_func_ << "_exit\n";
            return;
        }
        if (auto* call = dynamic_cast<const CallExpr*>(stmt)) {
            gen_call(call);
            return;
        }
        if (auto* ifs = dynamic_cast<const IfStmt*>(stmt)) {
            cse_clear();   // 分支点：跨分支的缓存不安全，整体清空
            int el = new_label(), en = new_label();
            gen_expr(ifs->cond.get());
            out_ << "    beqz t0, .L" << el << "\n";
            gen_stmt(ifs->then_stmt.get());
            out_ << "    j .L" << en << "\n";
            out_ << ".L" << el << ":\n";
            cse_clear();   // 分支间清空：else 分支不能复用 then 分支的缓存（运行时互斥）
            if (ifs->else_stmt) gen_stmt(ifs->else_stmt.get());
            out_ << ".L" << en << ":\n";
            cse_clear();   // 汇合点清空：if 之后不能复用任何分支内的缓存
            return;
        }
        if (auto* ws = dynamic_cast<const WhileStmt*>(stmt)) {
            cse_clear();   // 循环入口：跨迭代缓存置空，保证安全
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

    // ---- 复制传播 ----

    // 构造一个临时 IdExpr（复制传播读源变量用）
    std::unique_ptr<ASTNode> make_id(const std::string& name) {
        return std::make_unique<IdExpr>(name);
    }

    // 沿复制表追踪变量到最终源（防循环）
    std::string copy_source(const std::string& name) {
        std::string cur = name;
        std::unordered_set<std::string> seen;
        while (seen.insert(cur).second) {
            auto it = copy_tab_.find(cur);
            if (it == copy_tab_.end()) break;
            cur = it->second;
        }
        return cur;
    }

    // 变量 x 被赋值/声明后：清除 x 自身及所有"源为 x"的副本（值已过时）
    void invalidate_copy(const std::string& x) {
        copy_tab_.erase(x);
        for (auto it = copy_tab_.begin(); it != copy_tab_.end(); ) {
            if (it->second == x) it = copy_tab_.erase(it);
            else ++it;
        }
    }

    // ---- 表达式 ----

    void gen_expr(const ASTNode* expr) {
        if (auto* num = dynamic_cast<const NumberExpr*>(expr)) {
            out_ << "    li t0, " << num->value << "\n";
            return;
        }
        if (auto* id = dynamic_cast<const IdExpr*>(expr)) {
            // 复制传播：x = y 后读 x 直接用 y 的位置
            auto cp = copy_tab_.find(id->name);
            if (cp != copy_tab_.end()) { gen_expr(make_id(cp->second).get()); return; }
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
            // CSE（公共子表达式消除）：简单操作数的重复表达式 → 复用缓存结果
            std::string l, r;
            std::string key = cse_key(bin, l, r);
            if (!key.empty()) {
                auto it = cse_map_.find(key);
                if (it != cse_map_.end()) {
                    out_ << "    lw t0, " << (cse_base_ + it->second.slot * 4) << "(sp)\n";
                    return;
                }
                gen_bin_expr(bin);           // 首次计算，结果在 t0
                if (cse_count_ < CSE_SLOTS) {
                    int slot = cse_count_++;
                    out_ << "    sw t0, " << (cse_base_ + slot * 4) << "(sp)\n";
                    cse_map_[key] = {l, r, slot};
                }
                return;
            }
            gen_bin_expr(bin);
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

    // 生成二元运算，结果在 t0
    // 若表达式是 s 寄存器中的局部变量（非 const），返回 "sX"；否则空串
    std::string simple_var_reg(const ASTNode* e) {
        if (auto* id = dynamic_cast<const IdExpr*>(e)) {
            if (find_const(id->name)) return "";   // const 是立即数
            if (VarLoc* loc = find_var(id->name))
                if (loc->in_reg) return "s" + std::to_string(loc->reg);
        }
        return "";
    }

    // 直接生成 t0 = a op b（a/b 是寄存器名）
    void gen_bin_op2(const std::string& op, const std::string& a, const std::string& b) {
        if (op == "+") out_ << "    add t0, " << a << ", " << b << "\n";
        else if (op == "-") out_ << "    sub t0, " << a << ", " << b << "\n";
        else if (op == "*") out_ << "    mul t0, " << a << ", " << b << "\n";
        else if (op == "/") out_ << "    div t0, " << a << ", " << b << "\n";
        else if (op == "%") out_ << "    rem t0, " << a << ", " << b << "\n";
        else if (op == "<") out_ << "    slt t0, " << a << ", " << b << "\n";
        else if (op == ">") out_ << "    slt t0, " << b << ", " << a << "\n";
        else if (op == "<=") out_ << "    slt t0, " << b << ", " << a << "\n    xori t0, t0, 1\n";
        else if (op == ">=") out_ << "    slt t0, " << a << ", " << b << "\n    xori t0, t0, 1\n";
        else if (op == "==") out_ << "    sub t0, " << a << ", " << b << "\n    seqz t0, t0\n";
        else if (op == "!=") out_ << "    sub t0, " << a << ", " << b << "\n    snez t0, t0\n";
        else throw std::runtime_error("gen_bin_op2: unknown op " + op);
    }

    // 生成二元运算，结果在 t0；寄存器操作数直接操作，省 push/pop 内存往返
    void gen_bin_expr(const BinaryExpr* bin) {
        std::string lr = simple_var_reg(bin->left.get());
        std::string rr = simple_var_reg(bin->right.get());
        if (!lr.empty() && !rr.empty()) { gen_bin_op2(bin->op, lr, rr); return; }
        if (!rr.empty()) { gen_expr(bin->left.get()); gen_bin_op2(bin->op, "t0", rr); return; }
        if (!lr.empty()) { gen_expr(bin->right.get()); gen_bin_op2(bin->op, lr, "t0"); return; }
        gen_expr(bin->left.get());
        push_t0();
        gen_expr(bin->right.get());
        pop_to_t1();
        gen_bin_op(bin->op);
    }

    // ---- 函数调用 ----

    // 表达式里是否含函数调用（判断参数是否需要暂存）
    bool contains_call(const ASTNode* n) {
        if (dynamic_cast<const CallExpr*>(n)) return true;
        if (auto* b = dynamic_cast<const BinaryExpr*>(n))
            return contains_call(b->left.get()) || contains_call(b->right.get());
        if (auto* u = dynamic_cast<const UnaryExpr*>(n))
            return contains_call(u->expr.get());
        return false;
    }

    void gen_call(const CallExpr* call) {
        int n = (int)call->args.size();
        // 参数里含嵌套调用 → 全部暂存临时区，避免内层调用覆盖已就位的 a 寄存器
        bool has_call = false;
        for (auto& a : call->args)
            if (contains_call(a.get())) { has_call = true; break; }

        if (has_call) {
            int base = extra_stack_;
            for (int i = 0; i < n; i++) {
                gen_expr(call->args[i].get());
                extra_stack_ += 4;
                out_ << "    sw t0, " << (temp_base_ + extra_stack_) << "(sp)\n";
            }
            // 寄存器参数 a0-a7（全部求值完再从暂存区读回）
            for (int i = 0; i < std::min(n, 8); i++) {
                out_ << "    lw t0, " << (temp_base_ + base + (i + 1) * 4) << "(sp)\n";
                out_ << "    mv " << arg_reg(i) << ", t0\n";
            }
            // 溢出参数：存到 sp 负偏移（调用者栈下方，callee 会找到）
            for (int i = 8; i < n; i++) {
                out_ << "    lw t0, " << (temp_base_ + base + (i + 1) * 4) << "(sp)\n";
                out_ << "    sw t0, -" << ((i - 8 + 2) * 4) << "(sp)\n";  // -8, -12, ... 避开 ra 的 -4
            }
            extra_stack_ = base;  // 清理本层暂存
        } else {
            // 无嵌套调用：求值后直接移入 a 寄存器（快）
            for (int i = 0; i < std::min(n, 8); i++) {
                gen_expr(call->args[i].get());
                out_ << "    mv " << arg_reg(i) << ", t0\n";
            }
            // 溢出参数
            for (int i = 8; i < n; i++) {
                gen_expr(call->args[i].get());
                out_ << "    sw t0, -" << ((i - 8 + 2) * 4) << "(sp)\n";
            }
        }
        out_ << "    call " << call->func_name << "\n";
        out_ << "    mv t0, a0\n";
    }

    // ---- CSE（公共子表达式消除） ----

    // 操作数标记：局部变量→"v名字"、常量→"c值"；全局→""（不做CSE，可能被调用改）
    std::string cse_operand(const ASTNode* e) {
        if (auto* n = dynamic_cast<const NumberExpr*>(e)) return "c" + std::to_string(n->value);
        if (auto* id = dynamic_cast<const IdExpr*>(e)) {
            if (global_vars_.count(id->name)) return "";
            if (copy_tab_.count(id->name)) return "";   // 复制源变量值跟随源，缓存失效难追踪
            return "v" + id->name;
        }
        return "";
    }

    // 构造 CSE 键；两个简单操作数才可缓存，key 为空表示不可 CSE
    std::string cse_key(const BinaryExpr* bin, std::string& l, std::string& r) {
        l = cse_operand(bin->left.get());
        r = cse_operand(bin->right.get());
        if (l.empty() || r.empty()) return "";
        if ((bin->op == "+" || bin->op == "*") && l > r) std::swap(l, r);  // 交换律统一顺序
        return bin->op + "|" + l + "|" + r;
    }

    // 变量 x 被赋值/声明后：删掉以 x 为操作数的缓存条目（值已过时）
    void cse_invalidate(const std::string& x) {
        std::string mark = "v" + x;
        for (auto it = cse_map_.begin(); it != cse_map_.end(); ) {
            if (it->second.l == mark || it->second.r == mark)
                it = cse_map_.erase(it);
            else ++it;
        }
    }

    void cse_clear() { cse_map_.clear(); }

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

    // ---- 临时栈（用帧内正偏移，函数调用不会覆盖） ----
    int temp_base_ = 0;  // 在 gen_func 中设置

    void push_t0() {
        extra_stack_ += 4;
        out_ << "    sw t0, " << (temp_base_ + extra_stack_) << "(sp)\n";
    }
    void pop_to_t1() {
        out_ << "    lw t1, " << (temp_base_ + extra_stack_) << "(sp)\n";
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
