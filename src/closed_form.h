// ============================================================
// 闭合式循环优化（closed-form loops）
//
// 把"循环累加固定表达式"算成一步：
//     while (i < N) { sum = sum + (A*i+B);  i = i + s; }
//     →  sum += A*Σi + B*T;  i += T*s;   （T = 迭代次数）
// 这是 gcc -O2 对 p08_copychain/p07_loopvar 等性能测试的主要提速手段。
// 只处理：单基本块纯计算循环、归纳变量常数步进、累加量是 iv 的常数线性函数。
// ============================================================

// 在 limit 之前找 v 最近一次定义并求常量值（沿 CONST/MOV/ADDI/ADD 链）
static bool eval_const_before(const IrFunc& f, int v, int limit, long long& out) {
    for (int i = limit - 1; i >= 0; i--) {
        if (f.insns[i].d != v) continue;
        const Insn& in = f.insns[i];
        switch (in.op) {
        case IROp::CONST: out = in.imm; return true;
        case IROp::MOV: return eval_const_before(f, in.a, i, out);
        case IROp::ADDI: { long long x; if (eval_const_before(f, in.a, i, x)) { out = x + in.imm; return true; } return false; }
        case IROp::ADD: { long long x, y; if (eval_const_before(f, in.a, i, x) && eval_const_before(f, in.b, i, y)) { out = x + y; return true; } return false; }
        case IROp::SUB: { long long x, y; if (eval_const_before(f, in.a, i, x) && eval_const_before(f, in.b, i, y)) { out = x - y; return true; } return false; }
        case IROp::NEG: { long long x; if (eval_const_before(f, in.a, i, x)) { out = -x; return true; } return false; }
        default: return false;
        }
    }
    return false;
}

// 识别并替换闭合式循环。返回是否修改。
static bool closed_form_loops(IrFunc& f) {
    auto ranges = bb_ranges(f);
    int nb = (int)ranges.size();
    if (nb < 3) return false;   // 至少 头+体+出口
    auto gconsts = compute_global_consts(f);

    bool changed = false;
    for (int hb = 0; hb + 2 < nb; hb++) {
        int hs = ranges[hb].first, he = ranges[hb].second;
        if (f.insns[hs].op != IROp::LABEL) continue;
        IROp hterm = f.insns[he - 1].op;
        // 支持三种头分支：融合后的 BGE/BLT，以及 SLT/SLTI+BZ/BNZ（未融合，立即数比较）
        if (hterm != IROp::BGE && hterm != IROp::BLT &&
            hterm != IROp::BZ && hterm != IROp::BNZ) continue;
        int header_label = f.insns[hs].label;
        int exitlbl = f.insns[he - 1].label;

        // 出口块必须在体块之后
        int exitblk = -1;
        for (int bi = 0; bi < nb; bi++)
            for (int i = ranges[bi].first; i < ranges[bi].second; i++)
                if (f.insns[i].op == IROp::LABEL && f.insns[i].label == exitlbl) exitblk = bi;
        if (exitblk != hb + 2) continue;   // 体块必须恰好一块，出口紧跟

        int bs = ranges[hb + 1].first, be = ranges[hb + 1].second;
        if (f.insns[be - 1].op != IROp::BR) continue;
        if (f.insns[be - 1].label != header_label) continue;   // 回边跳回头块标签

        // header 标签只能被回边引用一次（无 continue/外层跳入）
        int refs = 0;
        for (const Insn& in : f.insns) {
            bool isbr = in.op == IROp::BR || in.op == IROp::BZ || in.op == IROp::BNZ ||
                        in.op == IROp::BLT || in.op == IROp::BGE;
            if (isbr && in.label == header_label) refs++;
        }
        if (refs != 1) continue;

        // 头块剩余指令 & 体块必须是纯计算（无 CALL/STORE/LOAD/LA/分支/LABEL）
        bool pure = true;
        auto ispure = [](IROp op) {
            switch (op) {
            case IROp::ADD: case IROp::SUB: case IROp::MUL: case IROp::DIV:
            case IROp::REM: case IROp::SLT: case IROp::ADDI: case IROp::SLTI:
            case IROp::SLLI: case IROp::NEG: case IROp::SEQZ: case IROp::SNEZ:
            case IROp::MOV: case IROp::CONST:
                return true;
            default: return false;
            }
        };
        for (int i = hs + 1; i < he - 1; i++) if (!ispure(f.insns[i].op)) { pure = false; break; }
        for (int i = bs; i < be - 1; i++) if (!ispure(f.insns[i].op)) { pure = false; break; }
        if (!pure) continue;

        // 找归纳变量 iv 与步长 s：体块内唯一的 `iv = iv ± const` 定义
        int iv = -1; long long step = 0;
        for (int i = bs; i < be - 1; i++) {
            const Insn& in = f.insns[i];
            if (in.d < 0) continue;
            if (in.op == IROp::ADDI && in.a == in.d) { iv = in.d; step = in.imm; }
            else if (in.op == IROp::SUB && in.a == in.d) {
                long long c;
                if (eval_const_before(f, in.b, i, c)) { iv = in.d; step = -c; }
            } else if (in.op == IROp::ADD && in.a == in.d) {
                long long c;
                if (eval_const_before(f, in.b, i, c)) { iv = in.d; step = c; }
            }
        }
        if (iv < 0 || step == 0) continue;
        int ivcnt = 0;
        for (int i = bs; i < be - 1; i++) if (f.insns[i].d == iv) ivcnt++;
        if (ivcnt != 1) continue;   // iv 只被增量定义一次

        // 条件分支必须用到 iv，另一个操作数必须是常数边界 N
        // cmp：0=iv<N,1=iv<=N,2=iv>N,3=iv>=N
        const Insn& hc = f.insns[he - 1];
        int cmp = -1; long long N = 0;
        if (hterm == IROp::BGE || hterm == IROp::BLT) {
            if (hc.a != iv && hc.b != iv) continue;
            int other = (hc.a == iv) ? hc.b : hc.a;
            if (!eval_const_before(f, other, he - 1, N)) {
                if (auto it = gconsts.find(other); it != gconsts.end()) N = it->second;
                else continue;
            }
            if (hterm == IROp::BGE) cmp = (hc.a == iv) ? 0 : 2;      // BGE iv,N → while iv<N; BGE N,iv → while iv>N
            else cmp = (hc.a == iv) ? 3 : 1;                          // BLT iv,N → while iv>=N; BLT N,iv → while iv<=N
        } else {
            // BZ/BNZ：在头块里找定义分支操作数的 SLT/SLTI
            int condv = hc.a;
            int defi = -1;
            for (int i = he - 2; i >= hs; i--)
                if (f.insns[i].d == condv && (f.insns[i].op == IROp::SLT || f.insns[i].op == IROp::SLTI)) { defi = i; break; }
            if (defi < 0) continue;
            const Insn& sl = f.insns[defi];
            if (sl.op == IROp::SLT) {
                int x = sl.a, y = sl.b;
                if (x == iv) {
                    if (!eval_const_before(f, y, defi, N)) {
                        if (auto it = gconsts.find(y); it != gconsts.end()) N = it->second; else continue;
                    }
                    // SLT iv,N → cond=(iv<N)。BZ exit when cond==0 → loop while iv<N
                    cmp = (hterm == IROp::BZ) ? 0 : 3;
                } else if (y == iv) {
                    if (!eval_const_before(f, x, defi, N)) {
                        if (auto it = gconsts.find(x); it != gconsts.end()) N = it->second; else continue;
                    }
                    // SLT N,iv → cond=(N<iv)。BZ exit → loop while N<iv → iv>N
                    cmp = (hterm == IROp::BZ) ? 2 : 1;
                } else continue;
            } else { // SLTI d, x, imm → cond=(x<imm)，x 必须是 iv
                if (sl.a != iv) continue;
                N = sl.imm;
                cmp = (hterm == IROp::BZ) ? 0 : 3;
            }
        }
        if (cmp < 0) continue;

        // iv 初值 i0
        long long i0 = 0;
        if (!eval_const_before(f, iv, hs, i0)) continue;

        // 迭代次数 T
        long long T = 0;
        {
            if (cmp == 0) {
                if (step > 0) { if (i0 >= N) T = 0; else T = (N - i0 + step - 1) / step; }
                else { if (i0 < N) T = 0; else continue; }
            } else if (cmp == 1) {
                if (step > 0) { if (i0 > N) T = 0; else T = (N - i0) / step + 1; }
                else { if (i0 <= N) T = 0; else continue; }
            } else if (cmp == 2) {
                if (step < 0) { if (i0 <= N) T = 0; else T = (i0 - N + (-step) - 1) / (-step); }
                else { if (i0 > N) T = 0; else continue; }
            } else {
                if (step < 0) { if (i0 < N) T = 0; else T = (i0 - N) / (-step) + 1; }
                else { if (i0 >= N) T = 0; else continue; }
            }
        }

        // 收集循环后仍存活的 vreg（体块之后被引用）
        std::unordered_set<int> live_after;
        for (int i = be; i < (int)f.insns.size(); i++) {
            const Insn& in = f.insns[i];
            if (in.a >= 0) live_after.insert(in.a);
            if (in.b >= 0) live_after.insert(in.b);
            for (int v : in.args) live_after.insert(v);
        }

        // 候选累加器：body 内定义、循环后存活、且 ≠ iv 的 vreg
        std::vector<int> cands;
        for (int i = bs; i < be - 1; i++) {
            int d = f.insns[i].d;
            if (d >= 0 && d != iv && live_after.count(d)) cands.push_back(d);
        }

        // 线性分析：对每个候选 acc，计算 (c_iv, c_acc, B)
        std::vector<std::pair<int, long long>> acc_deltas;   // (acc, 总增量)
        bool ok = true;
        for (int acc : cands) {
            std::unordered_map<int, std::tuple<long long, long long, long long>> form;
            bool fail = false;
            for (int i = bs; i < be - 1 && !fail; i++) {
                const Insn& in = f.insns[i];
                if (in.d < 0) continue;
                auto opform = [&](int v, long long& ci, long long& ca, long long& B) -> bool {
                    if (v == iv) { ci = 1; ca = 0; B = 0; return true; }
                    if (v == acc) { ci = 0; ca = 1; B = 0; return true; }
                    auto it = form.find(v);
                    if (it != form.end()) { std::tie(ci, ca, B) = it->second; return true; }
                    long long c;
                    if (eval_const_before(f, v, i, c)) { ci = 0; ca = 0; B = c; return true; }
                    if (auto gc = gconsts.find(v); gc != gconsts.end()) { ci = 0; ca = 0; B = gc->second; return true; }
                    return false;   // 其它符号 → 无法闭合
                };
                long long ci = 0, ca = 0, B = 0;
                bool known = false;
                switch (in.op) {
                case IROp::CONST: ci = 0; ca = 0; B = in.imm; known = true; break;
                case IROp::MOV:   known = opform(in.a, ci, ca, B); break;
                case IROp::ADDI: {
                    long long x1, x2, x3;
                    if (opform(in.a, x1, x2, x3)) { ci = x1; ca = x2; B = x3 + in.imm; known = true; }
                    break;
                }
                case IROp::ADD: {
                    long long a1, a2, a3, b1, b2, b3;
                    if (opform(in.a, a1, a2, a3) && opform(in.b, b1, b2, b3)) {
                        ci = a1 + b1; ca = a2 + b2; B = a3 + b3; known = true;
                    }
                    break;
                }
                case IROp::SUB: {
                    long long a1, a2, a3, b1, b2, b3;
                    if (opform(in.a, a1, a2, a3) && opform(in.b, b1, b2, b3)) {
                        ci = a1 - b1; ca = a2 - b2; B = a3 - b3; known = true;
                    }
                    break;
                }
                case IROp::MUL: {
                    long long a1, a2, a3, b1, b2, b3;
                    if (opform(in.a, a1, a2, a3) && opform(in.b, b1, b2, b3)) {
                        if (a1 == 0 && a2 == 0) { ci = b1 * a3; ca = b2 * a3; B = b3 * a3; known = true; }
                        else if (b1 == 0 && b2 == 0) { ci = a1 * b3; ca = a2 * b3; B = a3 * b3; known = true; }
                    }
                    break;
                }
                case IROp::SLLI: {
                    long long x1, x2, x3;
                    if (opform(in.a, x1, x2, x3)) { ci = x1 << in.imm; ca = x2 << in.imm; B = x3 << in.imm; known = true; }
                    break;
                }
                case IROp::NEG: {
                    long long x1, x2, x3;
                    if (opform(in.a, x1, x2, x3)) { ci = -x1; ca = -x2; B = -x3; known = true; }
                    break;
                }
                case IROp::SLTI: {
                    long long x1, x2, x3;
                    if (opform(in.a, x1, x2, x3) && x1 == 0 && x2 == 0) { B = (x3 < in.imm) ? 1 : 0; ci = 0; ca = 0; known = true; }
                    break;
                }
                case IROp::SLT: {
                    long long a1, a2, a3, b1, b2, b3;
                    if (opform(in.a, a1, a2, a3) && opform(in.b, b1, b2, b3) &&
                        a1 == 0 && a2 == 0 && b1 == 0 && b2 == 0) {
                        B = (a3 < b3) ? 1 : 0; ci = 0; ca = 0; known = true;
                    }
                    break;
                }
                case IROp::SEQZ: case IROp::SNEZ: {
                    long long x1, x2, x3;
                    if (opform(in.a, x1, x2, x3) && x1 == 0 && x2 == 0) {
                        B = (in.op == IROp::SEQZ) ? (x3 == 0 ? 1 : 0) : (x3 != 0 ? 1 : 0);
                        ci = 0; ca = 0; known = true;
                    }
                    break;
                }
                default: break;
                }
                if (!known) { fail = true; break; }
                form[in.d] = {ci, ca, B};
            }
            if (fail) { ok = false; break; }
            auto it = form.find(acc);
            if (it == form.end()) { ok = false; break; }
            auto [ci, ca, B] = it->second;
            if (ca != 1) { ok = false; break; }   // 必须纯累加（系数 1）
            // 增量 = Σ_{k=0}^{T-1} (ci*(i0+k*step) + B)
            long long inc = ci * T * i0 + ci * step * T * (T - 1) / 2 + B * T;
            acc_deltas.push_back({acc, inc});
        }
        if (!ok) continue;

        // 生成替换指令
        std::vector<Insn> repl;
        int fresh = 0;
        for (const Insn& in : f.insns) fresh = std::max(fresh, in.d + 1);
        for (int p : f.params) fresh = std::max(fresh, p + 1);
        for (auto& [acc, inc] : acc_deltas) {
            int t = fresh++;
            int v = (int)(uint64_t)inc;   // 截断为 32 位
            repl.push_back({IROp::CONST, t, -1, -1, v});
            repl.push_back({IROp::ADD, acc, acc, t});
        }
        if (live_after.count(iv)) {
            int t = fresh++;
            long long delta = (long long)T * step;
            int v = (int)(uint64_t)delta;
            repl.push_back({IROp::CONST, t, -1, -1, v});
            repl.push_back({IROp::ADD, iv, iv, t});
        }
        repl.push_back({IROp::BR, -1, -1, -1, 0, exitlbl});
        f.insns.erase(f.insns.begin() + hs, f.insns.begin() + be);
        f.insns.insert(f.insns.begin() + hs, repl.begin(), repl.end());
        changed = true;
        hb += 2;   // 跳过头块（已被替换），避免重复处理
    }
    return changed;
}
