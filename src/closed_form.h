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
        int brtgt = f.insns[he - 1].label;

        // 两种布局：
        //   正常：头块条件分支→出口；体块 hb+1 以 BR 回头块；出口 hb+2 以标签开头
        //   反转：头块条件分支→体块；出口 hb+1 为 fall-through（返回代码）；体块 hb+2
        // 先找"以 BR 回头块标签结尾"的块 → 那就是体块
        // 体块 = 以 BR 回头块标签结尾的块（可能在 hb+1..hb+3，因空标签块可能拆分）
        int bodyblk = -1;
        for (int bi = hb + 1; bi < nb && bi <= hb + 3; bi++) {
            int ei = ranges[bi].second - 1;
            if (f.insns[ei].op == IROp::BR && f.insns[ei].label == header_label) { bodyblk = bi; break; }
        }
        if (bodyblk < 0) continue;

        // 分支目标块：决定布局
        int tgtblk = -1;
        for (int bi = 0; bi < nb; bi++)
            for (int i = ranges[bi].first; i < ranges[bi].second; i++)
                if (f.insns[i].op == IROp::LABEL && f.insns[i].label == brtgt) tgtblk = bi;
        if (tgtblk < 0) continue;
        // 反转布局：分支目标在体块或其前面的标签块（tgtblk <= bodyblk），
        //   出口在 hb+1..bodyblk-1 之间（fall-through 返回代码）
        // 正常布局：分支目标 = 出口（tgtblk == bodyblk+1，即紧跟在体块之后）
        bool inverted = (tgtblk <= bodyblk);
        if (!inverted && tgtblk != bodyblk + 1) continue;
        // 反转布局里，分支目标块若只是空标签块，其实际代码在下一个非空块——体块就在其后
        if (inverted && tgtblk < hb + 1) continue;

        int bs = ranges[bodyblk].first, be = ranges[bodyblk].second;
        // 出口块：
        //   反转：从 hb+1 到 bodyblk-1 的所有块（返回代码）合并为一个"出口"
        //   正常：bodyblk+1（分支目标块）
        int es = -1, ee = -1;
        if (inverted) {
            es = ranges[hb + 1].first;
            ee = ranges[bodyblk].first;   // 到体块前为止
            if (es >= ee) continue;
            // 出口块必须简单且以 BR/RET 结束（跳过结尾的空标签块）
            int eterm_i = ee - 1;
            while (eterm_i > es && f.insns[eterm_i].op == IROp::LABEL) eterm_i--;
            IROp eterm = f.insns[eterm_i].op;
            if (eterm != IROp::BR && eterm != IROp::RET) continue;
        } else {
            es = ranges[bodyblk + 1].first;
            ee = ranges[bodyblk + 1].second;
        }

        // header 标签只能被回边引用一次（无 continue/外层跳入）
        int refs = 0;
        for (const Insn& in : f.insns) {
            bool isbr = in.op == IROp::BR || in.op == IROp::BZ || in.op == IROp::BNZ ||
                        in.op == IROp::BLT || in.op == IROp::BGE;
            if (isbr && in.label == header_label) refs++;
        }
        if (refs != 1) continue;
        // 反转布局：分支目标（体块标签）只能被头块这一条分支引用
        if (inverted) {
            int brefs = 0;
            for (const Insn& in : f.insns) {
                bool isbr = in.op == IROp::BR || in.op == IROp::BZ || in.op == IROp::BNZ ||
                            in.op == IROp::BLT || in.op == IROp::BGE;
                if (isbr && in.label == brtgt) brefs++;
            }
            if (brefs != 1) continue;
        }

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
        for (int i = hs + 1; i < he - 1; i++)
            if (f.insns[i].op != IROp::LABEL && !ispure(f.insns[i].op)) { pure = false; break; }
        for (int i = bs; i < be - 1; i++)
            if (f.insns[i].op != IROp::LABEL && !ispure(f.insns[i].op)) { pure = false; break; }
        if (!pure) continue;

        // 找归纳变量 iv 与步长 s。
        // 关键：iv 必须出现在头块条件分支里。若体块有多个自增 vreg（累加器+归纳变量），
        // 先收集所有候选，再选"出现在条件分支操作数里"的那个。
        // 形态 1：`iv = iv ± const`
        // 形态 2（尾递归/临时链）：`temp = iv ± const; mv iv, temp`
        std::vector<std::pair<int, long long>> ivcands;   // (vreg, step)
        for (int i = bs; i < be - 1; i++) {
            const Insn& in = f.insns[i];
            if (in.d < 0) continue;
            long long st = 0; bool isiv = false;
            if (in.op == IROp::ADDI && in.a == in.d) { st = in.imm; isiv = true; }
            else if (in.op == IROp::SUB && in.a == in.d) {
                long long c;
                if (eval_const_before(f, in.b, i, c)) { st = -c; isiv = true; }
            } else if (in.op == IROp::ADD && in.a == in.d) {
                long long c;
                if (eval_const_before(f, in.b, i, c)) { st = c; isiv = true; }
            } else if (in.op == IROp::MOV && in.a != in.d) {
                for (int j = bs; j < i; j++) {
                    const Insn& jin = f.insns[j];
                    if (jin.d != in.a) continue;
                    if (jin.op == IROp::ADDI && jin.a == in.d) { st = jin.imm; isiv = true; }
                    else if (jin.op == IROp::ADD && jin.a == in.d) {
                        long long c;
                        if (eval_const_before(f, jin.b, j, c)) { st = c; isiv = true; }
                    }
                    break;
                }
            }
            if (isiv && st != 0) ivcands.push_back({in.d, st});
        }
        // 选条件分支里的那个作为 iv。
        // BGE/BLT：iv 直接是分支操作数之一。
        // BZ/BNZ：分支操作数是 SLT/SLTI 结果，iv 在定义该结果的比较指令里。
        const Insn& hcv = f.insns[he - 1];
        int iv = -1; long long step = 0;
        if (hcv.op == IROp::BGE || hcv.op == IROp::BLT) {
            for (auto& [v, st] : ivcands)
                if (hcv.a == v || hcv.b == v) { iv = v; step = st; break; }
        } else {
            // 找定义分支操作数的 SLT/SLTI/SEQZ 链，取其中出现的候选 iv
            int condv = hcv.a;
            int defi = -1;
            for (int i = he - 2; i >= hs; i--)
                if (f.insns[i].d == condv) { defi = i; break; }
            if (defi < 0) continue;
            int cand_operand = -1;
            if (f.insns[defi].op == IROp::SEQZ) {
                int slt_i = -1;
                for (int i = defi - 1; i >= hs; i--)
                    if (f.insns[i].d == f.insns[defi].a) { slt_i = i; break; }
                if (slt_i >= 0) cand_operand = slt_i;
            } else cand_operand = defi;
            if (cand_operand < 0) continue;
            const Insn& cmpin = f.insns[cand_operand];
            for (auto& [v, st] : ivcands)
                if (cmpin.a == v || cmpin.b == v) { iv = v; step = st; break; }
        }
        if (iv < 0) continue;
        int ivcnt = 0;
        for (int i = bs; i < be - 1; i++) if (f.insns[i].d == iv) ivcnt++;
        if (ivcnt != 1) continue;   // iv 只被增量定义一次

        // 条件分支必须用到 iv，另一个操作数必须是常数边界 N
        // cmp：0=iv<N,1=iv<=N,2=iv>N,3=iv>=N
        const Insn& hc = hcv;
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
            // BZ/BNZ：在头块里找定义分支操作数的比较指令。
            // 可能形态：SLT/SLTI 直接喂；或 SLT + SEQZ（等价 <= / >=）。
            int condv = hc.a;
            int defi = -1;
            for (int i = he - 2; i >= hs; i--)
                if (f.insns[i].d == condv && (f.insns[i].op == IROp::SLT || f.insns[i].op == IROp::SLTI ||
                                              f.insns[i].op == IROp::SEQZ)) { defi = i; break; }
            if (defi < 0) continue;
            const Insn& d0 = f.insns[defi];
            // 若最靠近分支的是 SEQZ，则往前找它操作数的 SLT/SLTI（即 <= / >= 形态）
            if (d0.op == IROp::SEQZ) {
                int slt_i = -1;
                for (int i = defi - 1; i >= hs; i--)
                    if (f.insns[i].d == d0.a && (f.insns[i].op == IROp::SLT || f.insns[i].op == IROp::SLTI)) { slt_i = i; break; }
                if (slt_i < 0) continue;
                const Insn& sl = f.insns[slt_i];
                int cmp_base = -1; long long Nb = 0;
                // 先算 SLT 的原始比较语义：SLT x,y → (x<y)
                if (sl.op == IROp::SLTI) {
                    if (sl.a != iv) continue;
                    Nb = sl.imm; cmp_base = 0;   // iv < imm
                } else {
                    if (sl.a == iv) {
                        if (!eval_const_before(f, sl.b, slt_i, Nb)) {
                            if (auto it = gconsts.find(sl.b); it != gconsts.end()) Nb = it->second; else continue;
                        }
                        cmp_base = 0;   // iv < N
                    } else if (sl.b == iv) {
                        if (!eval_const_before(f, sl.a, slt_i, Nb)) {
                            if (auto it = gconsts.find(sl.a); it != gconsts.end()) Nb = it->second; else continue;
                        }
                        cmp_base = 2;   // N < iv  (iv > N)
                    } else continue;
                }
                // seqz 把 (iv OP N) 取反 → 循环条件是 iv !OP N
                // seqz d, t 当 t==0 时 d=1；分支 BZ d: exit when d==0 → t!=0 → 原比较为真时循环
                // 原比较 cmp_base: 0=iv<N, 2=iv>N
                // seqz 取反：循环条件变为 (原比较) 的否
                // BZ d,exit: exit when !(原比较为真)... 需推导：
                //   t = (iv<N)。d = !t。BZ d → exit when d==0 → t==1 → iv<N 时退出
                //   循环 while !(iv<N) = iv>=N → cmp 3
                //   （BNZ d → exit when d!=0 → t==0 → iv<N 假 → 循环 while iv<N → cmp 0）
                // 同理 t=(iv>N)：d=!t。BZ → exit when t → iv>N 退出 → 循环 iv<=N → cmp 1
                //   BNZ → exit when !t → 循环 while iv>N → cmp 2
                int flipped;
                if (cmp_base == 0) flipped = (hterm == IROp::BZ) ? 3 : 0;   // iv<N → iv>=N / iv<N
                else flipped = (hterm == IROp::BZ) ? 1 : 2;                  // iv>N → iv<=N / iv>N
                cmp = flipped; N = Nb;
            } else {
                const Insn& sl = d0;
                if (sl.op == IROp::SLT) {
                    int x = sl.a, y = sl.b;
                    if (x == iv) {
                        if (!eval_const_before(f, y, defi, N)) {
                            if (auto it = gconsts.find(y); it != gconsts.end()) N = it->second; else continue;
                        }
                        cmp = (hterm == IROp::BZ) ? 0 : 3;
                    } else if (y == iv) {
                        if (!eval_const_before(f, x, defi, N)) {
                            if (auto it = gconsts.find(x); it != gconsts.end()) N = it->second; else continue;
                        }
                        cmp = (hterm == IROp::BZ) ? 2 : 1;
                    } else continue;
                } else { // SLTI d, x, imm
                    if (sl.a != iv) continue;
                    N = sl.imm;
                    cmp = (hterm == IROp::BZ) ? 0 : 3;
                }
            }
        }
        if (cmp < 0) continue;

        // 反转布局：条件分支跳进循环体（continue），fall-through 是出口。
        // 语义正好相反：正常布局里"分支=出口"，反转里"分支=继续循环"。
        if (inverted) cmp = 3 - cmp;   // iv<N↔iv>=N, iv<=N↔iv>N

        // iv 初值 i0（编译期常量）
        long long i0 = 0;
        bool i0_const = eval_const_before(f, iv, hs, i0);
        // 若 iv 初值不是常量，但能找到其来源 vreg（形参/前面计算）→ 运行时闭合式
        int i0v = -1;
        if (!i0_const) {
            // 若 iv 是形参，初值就是形参本身
            for (int p : f.params) if (p == iv) { i0v = iv; break; }
            if (i0v < 0) {
                for (int i = hs - 1; i >= 0; i--)
                    if (f.insns[i].d == iv) { if (f.insns[i].op == IROp::MOV) i0v = f.insns[i].a; break; }
            }
        }

        // 迭代次数 T
        long long T = 0;
        bool T_ok = true;
        if (i0_const) {
            if (cmp == 0) {
                if (step > 0) { if (i0 >= N) T = 0; else T = (N - i0 + step - 1) / step; }
                else T_ok = false;
            } else if (cmp == 1) {
                if (step > 0) { if (i0 > N) T = 0; else T = (N - i0) / step + 1; }
                else T_ok = false;
            } else if (cmp == 2) {
                if (step < 0) { if (i0 <= N) T = 0; else T = (i0 - N + (-step) - 1) / (-step); }
                else T_ok = false;
            } else {
                if (step < 0) { if (i0 < N) T = 0; else T = (i0 - N) / (-step) + 1; }
                else T_ok = false;
            }
        } else {
            // 运行时闭合式（i0 为参数）暂缓：先只做编译期可算的。
            // 保留 i0v 供将来实现，当前一律跳过（循环保留，结果仍正确）。
            T_ok = false;
        }
        if (!T_ok) continue;

        // 收集循环后仍存活的 vreg。
        // 正常布局：体块之后（be 起）。反转布局：出口块在体块之前的位置也要算（它运行在循环之后）。
        std::unordered_set<int> live_after;
        auto collect_uses = [&](int from, int to) {
            for (int i = from; i < to; i++) {
                const Insn& in = f.insns[i];
                if (in.a >= 0) live_after.insert(in.a);
                if (in.b >= 0) live_after.insert(in.b);
                for (int v : in.args) live_after.insert(v);
            }
        };
        if (inverted) collect_uses(es, ee);   // 出口块（返回代码）运行在循环后
        collect_uses(be, (int)f.insns.size());

        // 候选累加器：body 内定义、循环后存活、且 ≠ iv 的 vreg
        std::vector<int> cands;
        for (int i = bs; i < be - 1; i++) {
            int d = f.insns[i].d;
            if (d >= 0 && d != iv && live_after.count(d)) cands.push_back(d);
        }

        // 线性分析：对每个候选 acc，计算 (c_iv, c_acc, B)
        // 记录 (acc, ci, B)：每迭代增量 = ci*iv + B
        std::vector<std::tuple<int, long long, long long>> acc_deltas;
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
            // 运行时闭合式只支持常数增量（ci==0）；否则编译期需 i0 常量
            if (!i0_const && ci != 0) { ok = false; break; }
            acc_deltas.push_back({acc, ci, B});
        }
        if (!ok) continue;

        // 生成替换指令
        std::vector<Insn> repl;
        int fresh = 0;
        for (const Insn& in : f.insns) fresh = std::max(fresh, in.d + 1);
        for (int p : f.params) fresh = std::max(fresh, p + 1);
        {
            for (auto& [acc, ci, B] : acc_deltas) {
                int t = fresh++;
                // 编译期总增量 = Σ_{k=0}^{T-1}(ci*(i0+k*step)+B)
                long long total = ci * T * i0 + ci * step * T * (T - 1) / 2 + B * T;
                int v = (int)(uint64_t)total;
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
            if (inverted) {
                // 反转布局：出口块（返回代码）运行在循环之后，原样保留其指令
                for (int i = es; i < ee; i++) repl.push_back(f.insns[i]);
            } else {
                repl.push_back({IROp::BR, -1, -1, -1, 0, brtgt});   // 正常布局：跳到出口
            }
        }
        f.insns.erase(f.insns.begin() + hs, f.insns.begin() + be);
        f.insns.insert(f.insns.begin() + hs, repl.begin(), repl.end());
        changed = true;
        hb += 2;   // 跳过头块（已被替换），避免重复处理
    }
    return changed;
}
