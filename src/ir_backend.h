#pragma once

// ============================================================
// IR 后端：AST → IR → 优化 → 寄存器分配 → RISC-V 汇编
//
// 设计：ToyC 无数组、无指针，因此所有局部变量都住在虚拟寄存器里。
// IR 是三地址码，操作数是无上限的虚拟寄存器（int 句柄）+ 立即数。
// 优化（常量折叠/复制传播/CSE/DCE）在 IR 上做，安全且无访存开销；
// 最后一次性线性扫描寄存器分配，再发射汇编。
// 全代码用整型句柄，杜绝悬垂指针（修掉旧 codegen 的 UB）。
// ============================================================

#include "ast.h"
#include <algorithm>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace toyc {

// ---------- IR 指令 ----------

enum class IROp {
    CONST,   // d = imm
    MOV,     // d = a
    ADD, SUB, MUL, DIV, REM,   // d = a op b
    SLT,     // d = (a < b) 有符号
    ADDI,    // d = a + imm
    SLTI,    // d = (a < imm) 有符号
    SLLI,    // d = a << imm
    NEG,     // d = -a
    SEQZ,    // d = (a == 0)
    SNEZ,    // d = (a != 0)
    LA,      // d = &global
    LOAD,    // d = mem[a + imm]
    STORE,   // mem[b + imm] = a
    CALL,    // d = f(args...); d=-1 = void
    BZ,      // if (a == 0) goto label
    BNZ,     // if (a != 0) goto label
    BR,      // goto label
    RET,     // return a (a=-1 = void)
    LABEL,   // 标签
    NOP,     // 空指令（优化产物，发射时忽略）
};

struct Insn {
    IROp op;
    int d = -1;            // 目标虚拟寄存器（-1 = 无）
    int a = -1;            // 操作数 1
    int b = -1;            // 操作数 2
    int imm = 0;           // 立即数 / LOAD/STORE 偏移
    int label = -1;        // 跳转目标 / 标签号
    std::string name;      // CALL 函数名 / LA 全局名
    std::vector<int> args; // CALL 参数虚拟寄存器

    Insn() = default;
    Insn(IROp o, int dd, int aa = -1, int bb = -1, int ii = 0, int ll = -1,
         std::string nn = "")
        : op(o), d(dd), a(aa), b(bb), imm(ii), label(ll), name(std::move(nn)) {}
};

// 一个函数的 IR
struct IrFunc {
    std::string name;
    std::vector<Insn> insns;
    std::vector<int> params;   // 形参虚拟寄存器（槽）
    int param_count = 0;
    int entry_label = -1;      // 尾递归跳回点（函数体第一条）
};

// ---------- AST → IR 降级 ----------

class IrBuilder {
public:
    explicit IrBuilder(const CompUnit& unit) : unit_(unit) {
        // 全局常量编译期求值（供读取时折叠）
        for (auto& g : unit_.globals)
            if (auto* cd = dynamic_cast<const ConstDecl*>(g.get()))
                global_const_[cd->name] = eval_cexpr(cd->init.get());
        // 记录全部全局名（区分常量/变量，赋值时使常量失效）
        for (auto& g : unit_.globals) {
            if (auto* vd = dynamic_cast<const VarDecl*>(g.get()))
                global_names_.insert(vd->name);
            else if (auto* cd = dynamic_cast<const ConstDecl*>(g.get()))
                global_names_.insert(cd->name);
        }
    }

    IrFunc lower(const FuncDef* func) {
        cur_ = IrFunc();
        cur_.name = func->name;
        cur_.param_count = (int)func->params.size();
        next_vreg_ = 0;
        next_label_ = 0;
        scopes_.clear();
        scopes_.push_back({});
        loops_.clear();
        local_const_.clear();
        local_invalidated_.clear();
        for (auto& p : func->params) {
            int s = new_vreg();
            scopes_.back()[p] = s;
            cur_.params.push_back(s);
        }
        cur_.entry_label = new_label();
        emit({IROp::LABEL, -1, -1, -1, 0, cur_.entry_label});
        for (auto& s : func->body->stmts) lower_stmt(s.get());
        return std::move(cur_);
    }

private:
    const CompUnit& unit_;
    std::unordered_map<std::string, int> global_const_;
    std::unordered_set<std::string> global_names_;
    std::unordered_set<std::string> global_invalidated_;  // 被赋值过的全局（常量失效）
    std::unordered_map<std::string, int> local_const_;    // 局部常量 → 值（跨块折叠）
    std::unordered_set<std::string> local_invalidated_;   // 被赋值过的局部常量
    int next_vreg_ = 0, next_label_ = 0;
    IrFunc cur_;
    std::vector<std::unordered_map<std::string, int>> scopes_;  // 变量 → 槽寄存器
    std::vector<std::pair<int, int>> loops_;  // {begin_label, end_label}

    int new_vreg() { return next_vreg_++; }
    int new_label() { return next_label_++; }
    void emit(const Insn& i) { cur_.insns.push_back(i); }

    // 查变量槽（从内到外），返回槽寄存器值（不存在返回 -1）
    int find_var(const std::string& n) const {
        for (int i = (int)scopes_.size() - 1; i >= 0; i--) {
            auto it = scopes_[i].find(n);
            if (it != scopes_[i].end()) return it->second;
        }
        return -1;
    }

    // 局部常量表达式求值（解析数字/局部常量/全局常量/算术），失败返回 nullopt
    std::optional<int> eval_local_const(const ASTNode* e) const {
        if (auto* num = dynamic_cast<const NumberExpr*>(e)) return num->value;
        if (auto* id = dynamic_cast<const IdExpr*>(e)) {
            auto li = local_const_.find(id->name);
            if (li != local_const_.end() && !local_invalidated_.count(id->name)) return li->second;
            auto gi = global_const_.find(id->name);
            if (gi != global_const_.end() && !global_invalidated_.count(id->name)) return gi->second;
            return std::nullopt;
        }
        if (auto* un = dynamic_cast<const UnaryExpr*>(e)) {
            auto v = eval_local_const(un->expr.get());
            if (!v) return std::nullopt;
            if (un->op == "-") return -*v;
            if (un->op == "+") return *v;
            if (un->op == "!") return (*v == 0) ? 1 : 0;
            return std::nullopt;
        }
        if (auto* bin = dynamic_cast<const BinaryExpr*>(e)) {
            auto l = eval_local_const(bin->left.get());
            auto r = eval_local_const(bin->right.get());
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

    // 全局常量表达式求值（只允许常量算术）
    int eval_cexpr(const ASTNode* e) const {
        if (auto* num = dynamic_cast<const NumberExpr*>(e)) return num->value;
        if (auto* id = dynamic_cast<const IdExpr*>(e)) {
            auto it = global_const_.find(id->name);
            if (it != global_const_.end()) return it->second;
            throw std::runtime_error("const init requires compile-time value: " + id->name);
        }
        if (auto* un = dynamic_cast<const UnaryExpr*>(e)) {
            int v = eval_cexpr(un->expr.get());
            if (un->op == "-") return -v;
            if (un->op == "!") return !v;
            if (un->op == "+") return v;
        }
        if (auto* bin = dynamic_cast<const BinaryExpr*>(e)) {
            int l = eval_cexpr(bin->left.get());
            int r = eval_cexpr(bin->right.get());
            const std::string& op = bin->op;
            if (op == "+") return l + r;
            if (op == "-") return l - r;
            if (op == "*") return l * r;
            if (op == "/") return l / r;
            if (op == "%") return l % r;
        }
        throw std::runtime_error("non-const expr in const init");
    }

    // ---- 表达式 ----

    int lower_expr(const ASTNode* e) {
        if (auto* num = dynamic_cast<const NumberExpr*>(e)) {
            int d = new_vreg();
            emit({IROp::CONST, d, -1, -1, num->value});
            return d;
        }
        if (auto* id = dynamic_cast<const IdExpr*>(e)) {
            // 局部常量（跨块折叠）：值恒定且未被赋值 → 直接 CONST
            {
                auto lc = local_const_.find(id->name);
                if (lc != local_const_.end() && !local_invalidated_.count(id->name)) {
                    int d = new_vreg();
                    emit({IROp::CONST, d, -1, -1, lc->second});
                    return d;
                }
            }
            int slot = find_var(id->name);
            if (slot >= 0) return slot;   // 局部变量 → 直接读槽
            auto gc = global_const_.find(id->name);
            if (gc != global_const_.end() && !global_invalidated_.count(id->name)) {
                int d = new_vreg();
                emit({IROp::CONST, d, -1, -1, gc->second});
                return d;
            }
            if (global_names_.count(id->name)) {   // 全局变量 → 内存读
                int addr = new_vreg();
                emit({IROp::LA, addr, -1, -1, 0, -1, id->name});
                int d = new_vreg();
                emit({IROp::LOAD, d, addr});
                return d;
            }
            throw std::runtime_error("undefined: " + id->name);
        }
        if (auto* un = dynamic_cast<const UnaryExpr*>(e)) {
            int a = lower_expr(un->expr.get());
            int d = new_vreg();
            if (un->op == "-") emit({IROp::NEG, d, a});
            else if (un->op == "!") emit({IROp::SEQZ, d, a});
            else emit({IROp::MOV, d, a});   // 一元 +
            return d;
        }
        if (auto* call = dynamic_cast<const CallExpr*>(e)) {
            std::vector<int> args;
            for (auto& a : call->args) args.push_back(lower_expr(a.get()));
            int d = new_vreg();
            Insn in(IROp::CALL, d);
            in.name = call->func_name;
            in.args = std::move(args);
            emit(in);
            return d;
        }
        if (auto* bin = dynamic_cast<const BinaryExpr*>(e)) {
            if (bin->op == "&&") return lower_and(bin);
            if (bin->op == "||") return lower_or(bin);
            int l = lower_expr(bin->left.get());
            int r = lower_expr(bin->right.get());
            int d = new_vreg();
            lower_binop(bin->op, d, l, r);
            return d;
        }
        throw std::runtime_error("bad expression");
    }

    void lower_binop(const std::string& op, int d, int l, int r) {
        if (op == "+") emit({IROp::ADD, d, l, r});
        else if (op == "-") emit({IROp::SUB, d, l, r});
        else if (op == "*") emit({IROp::MUL, d, l, r});
        else if (op == "/") emit({IROp::DIV, d, l, r});
        else if (op == "%") emit({IROp::REM, d, l, r});
        else if (op == "<") emit({IROp::SLT, d, l, r});
        else if (op == ">") emit({IROp::SLT, d, r, l});
        else if (op == "<=") { int t = new_vreg(); emit({IROp::SLT, t, r, l}); emit({IROp::SEQZ, d, t}); }
        else if (op == ">=") { int t = new_vreg(); emit({IROp::SLT, t, l, r}); emit({IROp::SEQZ, d, t}); }
        else if (op == "==") { int t = new_vreg(); emit({IROp::SUB, t, l, r}); emit({IROp::SEQZ, d, t}); }
        else if (op == "!=") { int t = new_vreg(); emit({IROp::SUB, t, l, r}); emit({IROp::SNEZ, d, t}); }
        else throw std::runtime_error("bad binop: " + op);
    }

    int lower_and(const BinaryExpr* b) {
        int l = lower_expr(b->left.get());
        int d = new_vreg();
        int lf = new_label(), end = new_label();
        emit({IROp::BZ, -1, l, -1, 0, lf});
        int r = lower_expr(b->right.get());
        emit({IROp::BZ, -1, r, -1, 0, lf});
        emit({IROp::CONST, d, -1, -1, 1});
        emit({IROp::BR, -1, -1, -1, 0, end});
        emit({IROp::LABEL, -1, -1, -1, 0, lf});
        emit({IROp::CONST, d, -1, -1, 0});
        emit({IROp::LABEL, -1, -1, -1, 0, end});
        return d;
    }
    int lower_or(const BinaryExpr* b) {
        int l = lower_expr(b->left.get());
        int d = new_vreg();
        int tr = new_label(), end = new_label();
        emit({IROp::BNZ, -1, l, -1, 0, tr});
        int r = lower_expr(b->right.get());
        emit({IROp::BNZ, -1, r, -1, 0, tr});
        emit({IROp::CONST, d, -1, -1, 0});
        emit({IROp::BR, -1, -1, -1, 0, end});
        emit({IROp::LABEL, -1, -1, -1, 0, tr});
        emit({IROp::CONST, d, -1, -1, 1});
        emit({IROp::LABEL, -1, -1, -1, 0, end});
        return d;
    }

    // ---- 语句 ----

    void lower_stmt(const ASTNode* s) {
        if (auto* b = dynamic_cast<const Block*>(s)) {
            scopes_.push_back({});
            for (auto& st : b->stmts) lower_stmt(st.get());
            scopes_.pop_back();
            return;
        }
        if (auto* vd = dynamic_cast<const VarDecl*>(s)) {
            int slot = new_vreg();
            scopes_.back()[vd->name] = slot;
            int v = lower_expr(vd->init.get());
            emit({IROp::MOV, slot, v});
            return;
        }
        if (auto* cd = dynamic_cast<const ConstDecl*>(s)) {
            int slot = new_vreg();
            scopes_.back()[cd->name] = slot;
            // 若初值可编译期求值 → 记录跨块常量（后续读取直接折叠）
            if (auto v = eval_local_const(cd->init.get()))
                local_const_[cd->name] = *v;
            int v = lower_expr(cd->init.get());
            emit({IROp::MOV, slot, v});
            return;
        }
        if (auto* as = dynamic_cast<const AssignStmt*>(s)) {
            // 给局部常量赋值 → 常量失效（转回普通变量槽）
            if (local_const_.count(as->name)) local_invalidated_.insert(as->name);
            int slot = find_var(as->name);
            if (slot >= 0) {
                int v = lower_expr(as->expr.get());
                emit({IROp::MOV, slot, v});
                return;
            }
            if (global_names_.count(as->name)) {
                global_invalidated_.insert(as->name);   // 全局被赋值：常量值失效
                int addr = new_vreg();
                emit({IROp::LA, addr, -1, -1, 0, -1, as->name});
                int v = lower_expr(as->expr.get());
                emit({IROp::STORE, -1, v, addr});
                return;
            }
            throw std::runtime_error("undefined: " + as->name);
        }
        if (auto* ret = dynamic_cast<const ReturnStmt*>(s)) {
            if (!ret->expr) { emit({IROp::RET, -1}); return; }
            // 尾递归：return 当前函数(args) → 绑定形参后跳回入口
            if (auto* call = dynamic_cast<const CallExpr*>(ret->expr.get())) {
                if (call->func_name == cur_.name &&
                    (int)call->args.size() == cur_.param_count) {
                    std::vector<int> args;
                    for (auto& a : call->args) args.push_back(lower_expr(a.get()));
                    for (size_t i = 0; i < args.size(); i++)
                        emit({IROp::MOV, cur_.params[i], args[i]});
                    emit({IROp::BR, -1, -1, -1, 0, cur_.entry_label});
                    return;
                }
            }
            int v = lower_expr(ret->expr.get());
            emit({IROp::RET, -1, v});
            return;
        }
        if (auto* call = dynamic_cast<const CallExpr*>(s)) {
            std::vector<int> args;
            for (auto& a : call->args) args.push_back(lower_expr(a.get()));
            Insn in(IROp::CALL, -1);
            in.name = call->func_name;
            in.args = std::move(args);
            emit(in);
            return;
        }
        if (auto* ifs = dynamic_cast<const IfStmt*>(s)) {
            int c = lower_expr(ifs->cond.get());
            int els = new_label(), end = new_label();
            emit({IROp::BZ, -1, c, -1, 0, els});
            lower_stmt(ifs->then_stmt.get());
            emit({IROp::BR, -1, -1, -1, 0, end});
            emit({IROp::LABEL, -1, -1, -1, 0, els});
            if (ifs->else_stmt) lower_stmt(ifs->else_stmt.get());
            emit({IROp::LABEL, -1, -1, -1, 0, end});
            return;
        }
        if (auto* ws = dynamic_cast<const WhileStmt*>(s)) {
            int bg = new_label(), en = new_label();
            loops_.push_back({bg, en});
            emit({IROp::LABEL, -1, -1, -1, 0, bg});
            int c = lower_expr(ws->cond.get());
            emit({IROp::BZ, -1, c, -1, 0, en});
            lower_stmt(ws->body.get());
            emit({IROp::BR, -1, -1, -1, 0, bg});
            emit({IROp::LABEL, -1, -1, -1, 0, en});
            loops_.pop_back();
            return;
        }
        if (dynamic_cast<const BreakStmt*>(s)) {
            emit({IROp::BR, -1, -1, -1, 0, loops_.back().second});
            return;
        }
        if (dynamic_cast<const ContinueStmt*>(s)) {
            emit({IROp::BR, -1, -1, -1, 0, loops_.back().first});
            return;
        }
        // 裸表达式语句
        lower_expr(s);
    }
};

// ---------- 优化 pass ----------

// 基本块区间（[start, end)）
static std::vector<std::pair<int, int>> bb_ranges(const IrFunc& f) {
    std::vector<std::pair<int, int>> out;
    const auto& v = f.insns;
    int start = 0;
    for (int i = 0; i < (int)v.size(); i++) {
        bool term = v[i].op == IROp::BR || v[i].op == IROp::BZ ||
                    v[i].op == IROp::BNZ || v[i].op == IROp::RET;
        bool next_label = (i + 1 < (int)v.size()) && v[i + 1].op == IROp::LABEL;
        if (term || next_label) {
            out.push_back({start, i + 1});
            start = i + 1;
        }
    }
    if (start < (int)v.size()) out.push_back({start, (int)v.size()});
    return out;
}

// 常量折叠+传播 + 代数化简 + 立即数融合（基本块内，前向）
static bool const_fold_bb(IrFunc& f, int s, int e) {
    bool changed = false;
    std::unordered_map<int, int> val;   // 虚拟寄存器 → 已知常量

    // 常量二元运算求值（除零/模零返回失败）
    auto fold2 = [](IROp op, long long l, long long r, int& out) {
        switch (op) {
        case IROp::ADD: out = (int)(l + r); return true;
        case IROp::SUB: out = (int)(l - r); return true;
        case IROp::MUL: out = (int)(l * r); return true;
        case IROp::DIV: if (r == 0) return false; out = (int)(l / r); return true;
        case IROp::REM: if (r == 0) return false; out = (int)(l % r); return true;
        case IROp::SLT: out = (l < r) ? 1 : 0; return true;
        default: return false;
        }
    };
    auto fits = [](long long v) { return v >= -2048 && v <= 2047; };
    auto pow2k = [](int v, int& k) {
        if (v > 0 && (v & (v - 1)) == 0) {
            int kk = 0; while (v > 1) { v >>= 1; kk++; } k = kk; return true;
        }
        return false;
    };
    // 改写后刷新 val[d]
    auto refresh = [&](Insn& in) {
        if (in.d < 0) return;
        switch (in.op) {
        case IROp::CONST: val[in.d] = in.imm; break;
        case IROp::MOV:
            if (in.a >= 0 && val.count(in.a)) val[in.d] = val[in.a];
            else val.erase(in.d);
            break;
        case IROp::ADDI:
            if (in.a >= 0 && val.count(in.a)) val[in.d] = val[in.a] + in.imm;
            else val.erase(in.d);
            break;
        case IROp::SLTI:
            if (in.a >= 0 && val.count(in.a)) val[in.d] = (val[in.a] < in.imm) ? 1 : 0;
            else val.erase(in.d);
            break;
        case IROp::SLLI:
            if (in.a >= 0 && val.count(in.a)) val[in.d] = val[in.a] << in.imm;
            else val.erase(in.d);
            break;
        default: val.erase(in.d);
        }
    };

    for (int i = s; i < e; i++) {
        Insn& in = f.insns[i];
        bool ka = in.a >= 0 && val.count(in.a);
        bool kb = in.b >= 0 && val.count(in.b);
        int av = ka ? val[in.a] : 0;
        int bv = kb ? val[in.b] : 0;

        switch (in.op) {
        case IROp::CONST: val[in.d] = in.imm; break;
        case IROp::MOV:
            refresh(in);
            break;
        case IROp::ADD: case IROp::SUB: case IROp::MUL:
        case IROp::DIV: case IROp::REM: case IROp::SLT: {
            bool done = false;
            if (ka && kb) {   // 两侧常量 → 直接折叠
                int r = 0;
                if (fold2(in.op, av, bv, r)) {
                    in.op = IROp::CONST; in.a = in.b = -1; in.imm = r;
                    val[in.d] = r; changed = true; break;
                }
            }
            // 代数化简 + 立即数融合
            IROp op = in.op;
            int rv = 0;
            if (op == IROp::ADD) {
                if (kb && bv == 0) { in.op = IROp::MOV; in.b = -1; done = true; }
                else if (ka && av == 0) { in.op = IROp::MOV; in.a = in.b; in.b = -1; done = true; }
                else if (kb && fits(bv)) { in.op = IROp::ADDI; in.b = -1; in.imm = bv; done = true; }
                else if (ka && fits(av)) { in.op = IROp::ADDI; in.a = in.b; in.b = -1; in.imm = av; done = true; }
            } else if (op == IROp::SUB) {
                if (kb && bv == 0) { in.op = IROp::MOV; in.b = -1; done = true; }
                else if (kb && fits(-(long long)bv)) { in.op = IROp::ADDI; in.b = -1; in.imm = -bv; done = true; }
            } else if (op == IROp::MUL) {
                if (kb && bv == 1) { in.op = IROp::MOV; in.b = -1; done = true; }
                else if (ka && av == 1) { in.op = IROp::MOV; in.a = in.b; in.b = -1; done = true; }
                else if (kb && bv == 0) { in.op = IROp::CONST; in.a = in.b = -1; in.imm = 0; done = true; }
                else if (ka && av == 0) { in.op = IROp::CONST; in.a = in.b = -1; in.imm = 0; done = true; }
                else if (kb && pow2k(bv, rv)) { in.op = IROp::SLLI; in.b = -1; in.imm = rv; done = true; }
                else if (ka && pow2k(av, rv)) { in.op = IROp::SLLI; in.a = in.b; in.b = -1; in.imm = rv; done = true; }
            } else if (op == IROp::DIV) {
                if (kb && bv == 1) { in.op = IROp::MOV; in.b = -1; done = true; }
            } else if (op == IROp::REM) {
                if (kb && bv == 1) { in.op = IROp::CONST; in.a = in.b = -1; in.imm = 0; done = true; }
            } else if (op == IROp::SLT) {
                if (kb && fits(bv)) { in.op = IROp::SLTI; in.b = -1; in.imm = bv; done = true; }
            }
            if (done) { refresh(in); changed = true; }
            else refresh(in);
            break;
        }
        case IROp::ADDI: case IROp::SLTI: case IROp::SLLI: {
            if (ka) {
                int r = 0; bool ok = false;
                if (in.op == IROp::ADDI) { r = val[in.a] + in.imm; ok = true; }
                else if (in.op == IROp::SLTI) { r = (val[in.a] < in.imm) ? 1 : 0; ok = true; }
                else if (in.op == IROp::SLLI) { r = val[in.a] << in.imm; ok = true; }
                if (ok) { in.op = IROp::CONST; in.a = in.b = -1; in.imm = r; val[in.d] = r; changed = true; }
                else val.erase(in.d);
            } else val.erase(in.d);
            break;
        }
        case IROp::NEG: case IROp::SEQZ: case IROp::SNEZ: {
            if (ka) {
                int r = (in.op == IROp::NEG) ? -val[in.a]
                      : (in.op == IROp::SEQZ) ? (val[in.a] == 0 ? 1 : 0)
                      : (val[in.a] != 0 ? 1 : 0);
                in.op = IROp::CONST; in.a = in.b = -1; in.imm = r;
                val[in.d] = r; changed = true;
            } else val.erase(in.d);
            break;
        }
        case IROp::BZ: case IROp::BNZ: {
            if (ka) {
                bool taken = (in.op == IROp::BZ) ? (val[in.a] == 0) : (val[in.a] != 0);
                if (taken) { in.op = IROp::BR; in.a = -1; }
                else { in.op = IROp::NOP; in.a = -1; in.label = -1; }
                changed = true;
            }
            break;
        }
        case IROp::LA: case IROp::LOAD: case IROp::STORE:
        case IROp::CALL: case IROp::BR: case IROp::RET: case IROp::LABEL:
        case IROp::NOP:
            if (in.d >= 0) val.erase(in.d);   // 定义未知值
            break;
        }
    }
    return changed;
}

// 复制传播（基本块内）：MOV y,x 后、x 被重定义前，读 y 换成读 x
static bool copy_prop_bb(IrFunc& f, int s, int e) {
    bool changed = false;
    std::unordered_map<int, int> cpy;   // y → x
    for (int i = s; i < e; i++) {
        Insn& in = f.insns[i];
        auto resolve = [&](int v) {
            for (int g = 0; g < 64; g++) {
                auto it = cpy.find(v);
                if (it == cpy.end()) return v;
                v = it->second;
            }
            return v;
        };
        auto repl = [&](int& v) {
            if (v >= 0) {
                auto it = cpy.find(v);
                if (it != cpy.end()) { v = resolve(v); changed = true; }
            }
        };
        repl(in.a); repl(in.b);
        for (auto& a : in.args) repl(a);
        // 定义：in.d 被重定义 → ① 以 in.d 为源的副本失效；② cpy[in.d] 自身失效
        //（非 MOV 重定义会让 cpy[in.d] 变陈旧，必须清除）
        if (in.d >= 0) {
            for (auto it = cpy.begin(); it != cpy.end();)
                if (it->second == in.d || it->first == in.d) it = cpy.erase(it); else ++it;
            if (in.op == IROp::MOV && in.a >= 0) {
                int src = resolve(in.a);
                if (src >= 0 && src != in.d) cpy[in.d] = src;
            }
        }
    }
    return changed;
}

// 公共子表达式消除（基本块内）：纯运算结果复用。
// 关键：缓存项的操作数被重新定义（槽重赋值）时缓存失效，否则会复用陈旧值。
// LOAD 缓存遇 STORE/CALL 也失效（内存可能被改）。
static bool cse_bb(IrFunc& f, int s, int e) {
    struct Entry { std::string key; int res; int op1, op2; bool load; };
    std::vector<Entry> tab;
    bool changed = false;
    for (int i = s; i < e; i++) {
        Insn& in = f.insns[i];
        std::string key;
        bool pure = true;
        bool is_load = false;
        switch (in.op) {
        case IROp::CONST: key = "C," + std::to_string(in.imm); break;
        case IROp::ADD: key = "A," + std::to_string(in.a) + "," + std::to_string(in.b); break;
        case IROp::SUB: key = "S," + std::to_string(in.a) + "," + std::to_string(in.b); break;
        case IROp::MUL: key = "M," + std::to_string(in.a) + "," + std::to_string(in.b); break;
        case IROp::DIV: key = "D," + std::to_string(in.a) + "," + std::to_string(in.b); break;
        case IROp::REM: key = "R," + std::to_string(in.a) + "," + std::to_string(in.b); break;
        case IROp::SLT: key = "L," + std::to_string(in.a) + "," + std::to_string(in.b); break;
        case IROp::ADDI: key = "I," + std::to_string(in.a) + "," + std::to_string(in.imm); break;
        case IROp::SLTI: key = "i," + std::to_string(in.a) + "," + std::to_string(in.imm); break;
        case IROp::SLLI: key = "l," + std::to_string(in.a) + "," + std::to_string(in.imm); break;
        case IROp::NEG: key = "N," + std::to_string(in.a); break;
        case IROp::SEQZ: key = "Z," + std::to_string(in.a); break;
        case IROp::SNEZ: key = "z," + std::to_string(in.a); break;
        case IROp::LA: key = "G," + in.name; break;
        case IROp::LOAD: key = "X," + std::to_string(in.a) + "," + std::to_string(in.imm); is_load = true; break;
        default: pure = false;
        }
        if (pure && in.d >= 0) {
            bool found = false;
            for (const Entry& en : tab) {
                if (en.key == key) {
                    if (en.res != in.d) {
                        in.op = IROp::MOV; in.a = en.res; in.b = -1; in.imm = 0;
                        changed = true;
                    }
                    found = true;
                    break;
                }
            }
            if (!found) tab.push_back({key, in.d, in.a, in.b, is_load});
        }
        // 失效：本指令定义 in.d → 清除所有用到 in.d 的缓存（避免陈旧复用）
        if (in.d >= 0) {
            tab.erase(std::remove_if(tab.begin(), tab.end(), [&](const Entry& en) {
                return en.op1 == in.d || en.op2 == in.d;
            }), tab.end());
        }
        // 失效：STORE/CALL 可能改内存 → 清除 LOAD 缓存
        if (in.op == IROp::STORE || in.op == IROp::CALL) {
            tab.erase(std::remove_if(tab.begin(), tab.end(), [](const Entry& en) {
                return en.load;
            }), tab.end());
        }
    }
    return changed;
}

// 是否为无副作用的"计算"指令（可直接把目标改成槽寄存器）。
// 注意：CONST/LA 不参与合并——若合并成 `CONST 槽, 值` 会被 LICM 当循环不变量
// 提出循环，把"循环内赋值"变成"只赋一次"。保持 CONST 定义纯临时值 + MOV 赋值，
// 使 LICM 外提纯临时值安全。
static bool is_pure_compute(IROp op) {
    switch (op) {
    case IROp::ADD: case IROp::SUB: case IROp::MUL: case IROp::DIV:
    case IROp::REM: case IROp::SLT: case IROp::ADDI: case IROp::SLTI:
    case IROp::SLLI: case IROp::NEG: case IROp::SEQZ: case IROp::SNEZ:
    case IROp::LOAD:
        return true;
    default:
        return false;
    }
}

// 直接目标寄存器合并：`<纯运算> temp; MOV slot, temp`（temp 仅此处使用）
// → `<纯运算> slot`，省掉赋值后的 mv。slot 在运算与 MOV 之间未被使用才安全。
static bool merge_mov_into_op(IrFunc& f) {
    std::unordered_map<int, int> uses;
    for (const Insn& in : f.insns) {
        if (in.a >= 0) uses[in.a]++;
        if (in.b >= 0) uses[in.b]++;
        for (int v : in.args) uses[v]++;
    }
    bool changed = false;
    for (size_t i = 0; i < f.insns.size(); i++) {
        Insn& in = f.insns[i];
        if (in.op != IROp::MOV || in.a < 0) continue;
        int temp = in.a, slot = in.d;
        if (uses[temp] != 1) continue;   // temp 只被这条 MOV 使用
        for (size_t j = i; j-- > 0;) {   // 从后往前找 temp 的定义
            if (f.insns[j].d != temp) continue;
            Insn& def = f.insns[j];
            if (!is_pure_compute(def.op)) break;
            // 检查 slot 在 (j, i) 之间既未被读取也未被重定义（避免覆盖/丢值）
            bool used = false;
            for (size_t k = j + 1; k < i; k++) {
                const Insn& m = f.insns[k];
                if (m.d == slot || m.a == slot || m.b == slot ||
                    std::find(m.args.begin(), m.args.end(), slot) != m.args.end()) {
                    used = true; break;
                }
            }
            if (used) break;
            def.d = slot;
            in.op = IROp::NOP;   // 删掉 MOV
            changed = true;
            break;
        }
    }
    return changed;
}

// LICM-常量外提：把循环内不变的 CONST/LA 指令移到函数入口。
// CONST/LA 无副作用，提前/无条件执行语义不变；省掉循环每次迭代的 li/la。
static void licm_const(IrFunc& f) {
    auto ranges = bb_ranges(f);
    int nb = (int)ranges.size();
    if (nb < 2) return;   // 单个块无循环
    std::unordered_map<int, int> lbl2blk;
    for (int bi = 0; bi < nb; bi++)
        for (int i = ranges[bi].first; i < ranges[bi].second; i++)
            if (f.insns[i].op == IROp::LABEL) lbl2blk[f.insns[i].label] = bi;
    std::vector<std::vector<int>> pred(nb), succ(nb);
    for (int bi = 0; bi < nb; bi++) {
        int last = ranges[bi].second - 1;
        IROp op = f.insns[last].op;
        auto link = [&](int a, int b) { succ[a].push_back(b); pred[b].push_back(a); };
        if (op == IROp::BZ || op == IROp::BNZ) {
            auto it = lbl2blk.find(f.insns[last].label);
            if (it != lbl2blk.end()) link(bi, it->second);
            if (bi + 1 < nb) link(bi, bi + 1);
        } else if (op == IROp::BR) {
            auto it = lbl2blk.find(f.insns[last].label);
            if (it != lbl2blk.end()) link(bi, it->second);
        } else if (op != IROp::RET && bi + 1 < nb) {
            link(bi, bi + 1);
        }
    }
    // 支配者（迭代数据流）
    std::vector<std::unordered_set<int>> dom(nb);
    dom[0].insert(0);
    for (int bi = 1; bi < nb; bi++) for (int i = 0; i < nb; i++) dom[bi].insert(i);
    bool ch = true;
    while (ch) {
        ch = false;
        for (int bi = 1; bi < nb; bi++) {
            std::unordered_set<int> nd;
            if (pred[bi].empty()) { nd.insert(bi); }
            else {
                nd = dom[pred[bi][0]];
                for (size_t pi = 1; pi < pred[bi].size(); pi++) {
                    std::unordered_set<int> inter;
                    for (int v : nd) if (dom[pred[bi][pi]].count(v)) inter.insert(v);
                    nd = std::move(inter);
                }
                nd.insert(bi);
            }
            if (nd != dom[bi]) { dom[bi] = std::move(nd); ch = true; }
        }
    }
    // 回边 → 标记循环内的块
    std::vector<char> in_loop(nb, 0);
    for (int u = 0; u < nb; u++)
        for (int v : succ[u]) {
            if (!dom[u].count(v)) continue;   // v 不支配 u → 非回边
            std::vector<char> lb(nb, 0);
            std::vector<int> st = {u};
            lb[u] = 1;
            while (!st.empty()) {
                int x = st.back(); st.pop_back();
                for (int p : pred[x]) {
                    if (p == v) continue;   // 不进 header
                    if (dom[p].count(v) && !lb[p]) { lb[p] = 1; st.push_back(p); }
                }
            }
            lb[v] = 1;
            for (int bi = 0; bi < nb; bi++) if (lb[bi]) in_loop[bi] = 1;
        }
    // 只外提"全函数仅定义一次"的 vreg 的 CONST/LA。
    // 关键：`&&`/`||` 的结果在不同分支被两次 CONST 定义（1 和 0），
    // 若都提出循环会让结果永远停在入口的初值。仅单次定义的值才真正不变量。
    std::unordered_map<int, int> def_count;
    for (const Insn& in : f.insns)
        if (in.d >= 0) def_count[in.d]++;
    std::vector<Insn> hoisted;
    for (int bi = 0; bi < nb; bi++) {
        if (!in_loop[bi]) continue;
        for (int i = ranges[bi].first; i < ranges[bi].second; i++) {
            Insn& in = f.insns[i];
            if ((in.op == IROp::CONST || in.op == IROp::LA) &&
                in.d >= 0 && def_count[in.d] == 1) {
                hoisted.push_back(in);
                in.op = IROp::NOP;
            }
        }
    }
    if (hoisted.empty()) return;
    auto it = f.insns.begin();
    while (it != f.insns.end() && it->op != IROp::LABEL) ++it;
    if (it == f.insns.end()) return;
    ++it;
    f.insns.insert(it, hoisted.begin(), hoisted.end());
}

// 死代码删除：先按可达性去掉不可达块，再删"结果未使用且无副作用"的指令
static void dce(IrFunc& f) {
    // 1. 基本块 + 可达性
    auto ranges = bb_ranges(f);
    std::unordered_map<int, int> lbl2blk;
    for (size_t bi = 0; bi < ranges.size(); bi++)
        for (int i = ranges[bi].first; i < ranges[bi].second; i++)
            if (f.insns[i].op == IROp::LABEL) lbl2blk[f.insns[i].label] = (int)bi;
    int nb = (int)ranges.size();
    std::vector<std::vector<int>> succ(nb);
    for (int bi = 0; bi < nb; bi++) {
        int last = ranges[bi].second - 1;
        IROp op = f.insns[last].op;
        if (op == IROp::BZ || op == IROp::BNZ) {
            auto it = lbl2blk.find(f.insns[last].label);
            if (it != lbl2blk.end()) succ[bi].push_back(it->second);
            if (bi + 1 < nb) succ[bi].push_back(bi + 1);
        } else if (op == IROp::BR) {
            auto it = lbl2blk.find(f.insns[last].label);
            if (it != lbl2blk.end()) succ[bi].push_back(it->second);
        } else if (op != IROp::RET && bi + 1 < nb) {
            succ[bi].push_back(bi + 1);   // RET 无后继；其余顺序下落
        }
    }
    std::vector<char> reach(nb, 0);
    std::vector<int> stack = {0};
    reach[0] = 1;
    while (!stack.empty()) {
        int b = stack.back(); stack.pop_back();
        for (int s : succ[b]) if (!reach[s]) { reach[s] = 1; stack.push_back(s); }
    }
    // 2. 只保留可达块
    std::vector<Insn> keep;
    for (size_t bi = 0; bi < ranges.size(); bi++)
        if (reach[bi])
            for (int i = ranges[bi].first; i < ranges[bi].second; i++)
                keep.push_back(f.insns[i]);
    f.insns = std::move(keep);

    // 3. 使用计数死代码删除
    std::unordered_map<int, int> uses;
    for (const Insn& in : f.insns) {
        if (in.a >= 0) uses[in.a]++;
        if (in.b >= 0) uses[in.b]++;
        for (int v : in.args) uses[v]++;
    }
    auto is_removable = [](const Insn& in) {
        switch (in.op) {
        case IROp::CALL: case IROp::STORE: case IROp::BZ: case IROp::BNZ:
        case IROp::BR: case IROp::RET: case IROp::LABEL:
            return false;
        default: return true;
        }
    };
    bool changed = true;
    while (changed) {
        changed = false;
        for (int i = 0; i < (int)f.insns.size();) {
            Insn& in = f.insns[i];
            if (in.op == IROp::NOP) {
                f.insns.erase(f.insns.begin() + i); changed = true; continue;
            }
            if (is_removable(in) && in.d >= 0 && uses[in.d] == 0) {
                if (in.a >= 0) uses[in.a]--;
                if (in.b >= 0) uses[in.b]--;
                for (int v : in.args) uses[v]--;
                f.insns.erase(f.insns.begin() + i); changed = true; continue;
            }
            i++;
        }
    }
}

// 主优化入口：多轮迭代到稳定
static void optimize_ir(IrFunc& f) {
    for (int round = 0; round < 4; round++) {
        bool changed = false;
        auto ranges = bb_ranges(f);
        for (auto& pr : ranges) {
            changed |= const_fold_bb(f, pr.first, pr.second);
            changed |= copy_prop_bb(f, pr.first, pr.second);
            changed |= cse_bb(f, pr.first, pr.second);
        }
        changed |= merge_mov_into_op(f);   // 消除赋值后的 mv
        licm_const(f);                     // 循环内常量外提
        dce(f);
        if (!changed) break;
    }
    dce(f);
}

// ---------- 寄存器分配 ----------

// 活跃分析：CFG 反向数据流，算出每条指令的 live_before / live_after。
// 这是正确分配的关键——循环回边会让形参槽多次重定义，单一的 [首定义,末使用]
// 区间会低估活跃性导致同一物理寄存器被分配给重叠的生命期。
struct Liveness {
    std::vector<std::unordered_set<int>> live_before;
    std::vector<std::unordered_set<int>> live_after;
};

static Liveness compute_liveness(const IrFunc& f) {
    auto ranges = bb_ranges(f);
    int nb = (int)ranges.size();
    std::unordered_map<int, int> lbl2blk;
    for (int bi = 0; bi < nb; bi++)
        for (int i = ranges[bi].first; i < ranges[bi].second; i++)
            if (f.insns[i].op == IROp::LABEL) lbl2blk[f.insns[i].label] = bi;
    std::vector<std::vector<int>> succ(nb);
    for (int bi = 0; bi < nb; bi++) {
        int last = ranges[bi].second - 1;
        IROp op = f.insns[last].op;
        if (op == IROp::BZ || op == IROp::BNZ) {
            auto it = lbl2blk.find(f.insns[last].label);
            if (it != lbl2blk.end()) succ[bi].push_back(it->second);
            if (bi + 1 < nb) succ[bi].push_back(bi + 1);
        } else if (op == IROp::BR) {
            auto it = lbl2blk.find(f.insns[last].label);
            if (it != lbl2blk.end()) succ[bi].push_back(it->second);
        } else if (op != IROp::RET && bi + 1 < nb) {
            succ[bi].push_back(bi + 1);   // RET 无后继；其余顺序下落
        }
    }
    std::vector<std::unordered_set<int>> gen(nb), kill(nb);
    for (int bi = 0; bi < nb; bi++) {
        std::unordered_set<int> defd;
        for (int i = ranges[bi].first; i < ranges[bi].second; i++) {
            const Insn& in = f.insns[i];
            auto use = [&](int v) { if (v >= 0 && !defd.count(v)) gen[bi].insert(v); };
            use(in.a); use(in.b);
            for (int v : in.args) use(v);
            if (in.d >= 0) { defd.insert(in.d); kill[bi].insert(in.d); }
        }
    }
    std::vector<std::unordered_set<int>> live_in(nb), live_out(nb);
    bool changed = true;
    while (changed) {
        changed = false;
        for (int bi = nb - 1; bi >= 0; bi--) {
            for (int s : succ[bi])
                for (int v : live_in[s])
                    if (!live_out[bi].count(v)) { live_out[bi].insert(v); changed = true; }
            std::unordered_set<int> li = gen[bi];
            for (int v : live_out[bi])
                if (!kill[bi].count(v)) li.insert(v);
            for (int v : li)
                if (!live_in[bi].count(v)) { live_in[bi].insert(v); changed = true; }
        }
    }
    Liveness L;
    int n = (int)f.insns.size();
    L.live_before.assign(n, {});
    L.live_after.assign(n, {});
    for (int bi = 0; bi < nb; bi++) {
        std::unordered_set<int> live = live_out[bi];
        for (int i = ranges[bi].second - 1; i >= ranges[bi].first; i--) {
            L.live_after[i] = live;
            const Insn& in = f.insns[i];
            if (in.d >= 0) live.erase(in.d);
            if (in.a >= 0) live.insert(in.a);
            if (in.b >= 0) live.insert(in.b);
            for (int v : in.args) live.insert(v);
            L.live_before[i] = live;
        }
    }
    return L;
}

// RISC-V ABI 寄存器名（下标 = 物理寄存器号）
static const char* REGNAME[32] = {
    "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2",
    "s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5",
    "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7",
    "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6"
};
static std::string regname(int p) { return REGNAME[p]; }

struct RegAlloc {
    std::unordered_map<int, int> phys;    // 虚拟寄存器 → 物理寄存器号
    std::unordered_set<int> spilled;      // 溢出的虚拟寄存器
    std::unordered_map<int, int> slot;    // 溢出 → 栈偏移
    int frame = 0;        // 帧大小（字节）
    int n_saved = 0;      // 需保存的 s 寄存器个数（s0..sK）
    int spill_bytes = 0;

    void run(const IrFunc& f) {
        static const std::vector<int> S = {8, 9, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27};  // s0-s11
        static const std::vector<int> T = {6, 7, 28, 29, 30};                              // t1-t5
        static std::vector<int> ST;   // T 优先，然后 S
        if (ST.empty()) { for (int p : T) ST.push_back(p); for (int p : S) ST.push_back(p); }

        Liveness L = compute_liveness(f);
        int n = (int)f.insns.size();

        // 跨调用：值在某条 CALL 之后仍存活 → 需要 s 寄存器（callee-saved）
        std::unordered_set<int> crossing;
        for (int i = 0; i < n; i++)
            if (f.insns[i].op == IROp::CALL)
                for (int v : L.live_after[i]) crossing.insert(v);

        // 精确活跃点 → 跨度 [first, last]
        std::unordered_map<int, int> first_pt, last_pt;
        for (int v : f.params) { first_pt[v] = 0; last_pt[v] = 0; }
        for (int i = 0; i < n; i++) {
            int d = f.insns[i].d;
            auto mark = [&](int v) { if (v < 0) return; if (!first_pt.count(v)) first_pt[v] = i; last_pt[v] = i; };
            for (int v : L.live_before[i]) mark(v);
            for (int v : L.live_after[i]) mark(v);
            if (d >= 0) mark(d);
        }

        // 溢出选择（sweep）：按 start 处理，超容量时溢出"最晚结束"的活跃 vreg
        // （长生命周期的槽寄存器优先被溢出，让短生命周期的临时值留在寄存器里）
        std::unordered_set<int> spill;
        auto select_spill = [&](const std::vector<int>& vregs, int cap) {
            std::vector<int> ord = vregs;
            std::sort(ord.begin(), ord.end(), [&](int x, int y) {
                if (first_pt[x] != first_pt[y]) return first_pt[x] < first_pt[y];
                return last_pt[x] < last_pt[y];
            });
            std::vector<std::pair<int, int>> active;   // (last_pt, vreg)
            for (int v : ord) {
                if (spill.count(v)) continue;   // 已在溢出集
                active.erase(std::remove_if(active.begin(), active.end(),
                    [&](const auto& pr) { return pr.first < first_pt[v]; }), active.end());
                active.push_back({last_pt[v], v});
                while ((int)active.size() > cap) {
                    auto it = std::max_element(active.begin(), active.end(),
                        [](const auto& a, const auto& b) { return a.first < b.first; });
                    spill.insert(it->second);
                    active.erase(it);
                }
            }
        };
        // 跨调用：S 池容量 12；全体：S+T 容量 17
        std::vector<int> cvregs;
        for (auto& kv : first_pt) if (crossing.count(kv.first)) cvregs.push_back(kv.first);
        select_spill(cvregs, 12);
        std::vector<int> allv;
        for (auto& kv : first_pt) allv.push_back(kv.first);
        select_spill(allv, 17);

        // 分配：跨调用 → S；非跨调用 → T 然后 S。一个 vreg 整个函数一个物理寄存器。
        auto assign_pool = [&](const std::vector<int>& vregs, const std::vector<int>& pool) {
            std::vector<int> ord = vregs;
            std::sort(ord.begin(), ord.end(), [&](int x, int y) {
                if (first_pt[x] != first_pt[y]) return first_pt[x] < first_pt[y];
                return last_pt[x] < last_pt[y];
            });
            int owner[32];
            for (int i = 0; i < 32; i++) owner[i] = -1;
            for (int v : ord) {
                if (spill.count(v)) continue;
                for (int p = 0; p < 32; p++)
                    if (owner[p] >= 0 && last_pt[owner[p]] < first_pt[v]) owner[p] = -1;
                int chosen = -1;
                for (int p : pool) if (owner[p] < 0) { chosen = p; break; }
                if (chosen >= 0) { phys[v] = chosen; owner[chosen] = v; }
                else spill.insert(v);   // 防御：仍溢出
            }
        };
        std::vector<int> cv, ncv;
        for (auto& kv : first_pt) {
            if (spill.count(kv.first)) continue;
            if (crossing.count(kv.first)) cv.push_back(kv.first);
            else ncv.push_back(kv.first);
        }
        assign_pool(cv, S);
        assign_pool(ncv, ST);

        // 溢出栈槽
        int sp = 0;
        for (int v : spill) { slot[v] = sp; sp += 4; }
        spilled = spill;
        spill_bytes = sp;

        // 保存的 s 寄存器个数（s0..sK）
        int maxs = -1;
        for (auto& kv : phys)
            for (int j = 0; j < 12; j++)
                if (kv.second == S[j] && j > maxs) maxs = j;
        n_saved = maxs + 1;

        // 帧 = 溢出 + 保存的 s + ra + 栈参数区
        int nstack = std::max(0, f.param_count - 8);
        frame = spill_bytes + n_saved * 4 + 4 + nstack * 4;
        if (frame < 16) frame = 16;
        frame = (frame + 15) & ~15;
    }
};

// ---------- 汇编发射 ----------

// 取操作数的物理寄存器名；若溢出则先 load 到 scratch 寄存器
static std::string resolve_op(std::ostringstream& out, const RegAlloc& ra, int v,
                              const char* scratch) {
    if (v < 0) return "zero";
    if (ra.spilled.count(v)) {
        out << "    lw " << scratch << ", " << ra.slot.at(v) << "(sp)\n";
        return scratch;
    }
    return regname(ra.phys.at(v));
}

static void emit_insn(std::ostringstream& out, const IrFunc& f, const RegAlloc& ra,
                      const Insn& in) {
    auto spilled = [&](int v) { return ra.spilled.count(v); };
    auto slot = [&](int v) { return ra.slot.at(v); };
    auto phys = [&](int v) { return ra.phys.at(v); };
    switch (in.op) {
    case IROp::LABEL:
        out << ".L" << f.name << "_" << in.label << ":\n";
        break;
    case IROp::BR:
        out << "    j .L" << f.name << "_" << in.label << "\n";
        break;
    case IROp::BZ: case IROp::BNZ: {
        std::string v = resolve_op(out, ra, in.a, "t0");
        out << "    " << (in.op == IROp::BZ ? "beqz" : "bnez") << " " << v
            << ", .L" << f.name << "_" << in.label << "\n";
        break;
    }
    case IROp::RET: {
        if (in.a >= 0) {
            std::string v = resolve_op(out, ra, in.a, "t0");
            out << "    mv a0, " << v << "\n";
        }
        out << "    j .L" << f.name << "_exit\n";
        break;
    }
    case IROp::CONST: {
        if (spilled(in.d)) out << "    li t0, " << in.imm << "\n    sw t0, " << slot(in.d) << "(sp)\n";
        else out << "    li " << regname(phys(in.d)) << ", " << in.imm << "\n";
        break;
    }
    case IROp::LA: {
        if (spilled(in.d)) out << "    la t0, " << in.name << "\n    sw t0, " << slot(in.d) << "(sp)\n";
        else out << "    la " << regname(phys(in.d)) << ", " << in.name << "\n";
        break;
    }
    case IROp::MOV: {
        std::string a = resolve_op(out, ra, in.a, "t0");
        if (spilled(in.d)) out << "    mv t0, " << a << "\n    sw t0, " << slot(in.d) << "(sp)\n";
        else out << "    mv " << regname(phys(in.d)) << ", " << a << "\n";
        break;
    }
    case IROp::ADD: case IROp::SUB: case IROp::MUL: case IROp::DIV:
    case IROp::REM: case IROp::SLT: {
        const char* opstr = in.op == IROp::ADD ? "add" : in.op == IROp::SUB ? "sub"
            : in.op == IROp::MUL ? "mul" : in.op == IROp::DIV ? "div"
            : in.op == IROp::REM ? "rem" : "slt";
        std::string a = resolve_op(out, ra, in.a, "t0");
        std::string b = resolve_op(out, ra, in.b, "t6");
        if (spilled(in.d))
            out << "    " << opstr << " t0, " << a << ", " << b << "\n    sw t0, " << slot(in.d) << "(sp)\n";
        else
            out << "    " << opstr << " " << regname(phys(in.d)) << ", " << a << ", " << b << "\n";
        break;
    }
    case IROp::ADDI: case IROp::SLTI: case IROp::SLLI: {
        const char* opstr = in.op == IROp::ADDI ? "addi" : in.op == IROp::SLTI ? "slti" : "slli";
        std::string a = resolve_op(out, ra, in.a, "t0");
        if (spilled(in.d))
            out << "    " << opstr << " t0, " << a << ", " << in.imm << "\n    sw t0, " << slot(in.d) << "(sp)\n";
        else
            out << "    " << opstr << " " << regname(phys(in.d)) << ", " << a << ", " << in.imm << "\n";
        break;
    }
    case IROp::NEG: case IROp::SEQZ: case IROp::SNEZ: {
        const char* opstr = in.op == IROp::NEG ? "neg" : in.op == IROp::SEQZ ? "seqz" : "snez";
        std::string a = resolve_op(out, ra, in.a, "t0");
        if (spilled(in.d))
            out << "    " << opstr << " t0, " << a << "\n    sw t0, " << slot(in.d) << "(sp)\n";
        else
            out << "    " << opstr << " " << regname(phys(in.d)) << ", " << a << "\n";
        break;
    }
    case IROp::LOAD: {
        std::string base = resolve_op(out, ra, in.a, "t0");
        if (spilled(in.d))
            out << "    lw t0, " << in.imm << "(" << base << ")\n    sw t0, " << slot(in.d) << "(sp)\n";
        else
            out << "    lw " << regname(phys(in.d)) << ", " << in.imm << "(" << base << ")\n";
        break;
    }
    case IROp::STORE: {
        std::string val = resolve_op(out, ra, in.a, "t0");
        std::string base = resolve_op(out, ra, in.b, "t6");
        out << "    sw " << val << ", " << in.imm << "(" << base << ")\n";
        break;
    }
    case IROp::CALL: {
        for (int i = 0; i < (int)in.args.size() && i < 8; i++) {
            std::string v = resolve_op(out, ra, in.args[i], "t0");
            out << "    mv a" << i << ", " << v << "\n";
        }
        for (int i = 8; i < (int)in.args.size(); i++) {
            std::string v = resolve_op(out, ra, in.args[i], "t0");
            out << "    sw " << v << ", -" << ((i - 8 + 2) * 4) << "(sp)\n";
        }
        out << "    call " << in.name << "\n";
        if (in.d >= 0) {
            if (spilled(in.d))
                out << "    mv t0, a0\n    sw t0, " << slot(in.d) << "(sp)\n";
            else
                out << "    mv " << regname(phys(in.d)) << ", a0\n";
        }
        break;
    }
    case IROp::NOP:
        break;
    }
}

// 整个函数：prologue + 函数体 + epilogue
static std::string emit_function(const IrFunc& f, const RegAlloc& ra) {
    std::ostringstream out;
    int frame = ra.frame;
    int save_base = ra.spill_bytes;
    int nstack = std::max(0, f.param_count - 8);

    out << ".globl " << f.name << "\n" << f.name << ":\n";
    out << "    addi sp, sp, -" << frame << "\n";
    out << "    sw ra, " << (frame - 4) << "(sp)\n";
    // 保存用到的 s 寄存器
    for (int j = 0; j < ra.n_saved; j++)
        out << "    sw s" << j << ", " << (save_base + j * 4) << "(sp)\n";
    // 绑定形参 a0-a7
    for (int i = 0; i < f.param_count && i < 8; i++) {
        int p = f.params[i];
        if (ra.spilled.count(p)) out << "    sw a" << i << ", " << ra.slot.at(p) << "(sp)\n";
        else out << "    mv " << regname(ra.phys.at(p)) << ", a" << i << "\n";
    }
    // 绑定栈参数（i>=8，从调用者帧读取）
    for (int i = 0; i < nstack; i++) {
        int p = f.params[8 + i];
        out << "    lw t0, " << (frame - (i + 2) * 4) << "(sp)\n";
        if (ra.spilled.count(p)) out << "    sw t0, " << ra.slot.at(p) << "(sp)\n";
        else out << "    mv " << regname(ra.phys.at(p)) << ", t0\n";
    }
    // 函数体
    for (const Insn& in : f.insns) emit_insn(out, f, ra, in);
    // epilogue
    out << ".L" << f.name << "_exit:\n";
    for (int j = ra.n_saved - 1; j >= 0; j--)
        out << "    lw s" << j << ", " << (save_base + j * 4) << "(sp)\n";
    out << "    lw ra, " << (frame - 4) << "(sp)\n";
    out << "    addi sp, sp, " << frame << "\n";
    out << "    ret\n\n";
    return out.str();
}

}  // namespace toyc
