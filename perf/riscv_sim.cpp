// 极简 RISC-V RV32I 解释器 —— 仅用于本地验证 ToyC 编译器输出的汇编
// 用法: riscv_sim < file.s   → 打印 main 返回值
// 也可用: compiler file.tc | riscv_sim   验证 -opt 生成代码的正确性
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
using namespace std;

static uint32_t R[32];            // 寄存器
static uint32_t pc = 0;
static uint8_t* mem;              // 内存
static const uint32_t MEMSZ = 64 * 1024 * 1024;      // 64MB
static const uint32_t STACK_BOTTOM = MEMSZ - 16;     // 栈从内存顶部向下
static const uint32_t GLOBALS_BASE = 1024;           // 全局变量放内存底部

struct Insn { string op; string a, b, c; int imm = 0; };

int regno(const string& s) {
    if (s.empty()) return 0;
    if (s[0] == 'x') return atoi(s.c_str() + 1);
    // ABI 名
    static const char* names[] = {"zero","ra","sp","gp","tp","t0","t1","t2",
        "s0","s1","a0","a1","a2","a3","a4","a5","a6","a7","s2","s3","s4",
        "s5","s6","s7","s8","s9","s10","s11","t3","t4","t5","t6"};
    for (int i = 0; i < 32; i++) if (s == names[i]) return i;
    return 0;
}

int32_t r(int i) { return (int32_t)R[i]; }

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    mem = (uint8_t*)calloc(MEMSZ, 1);
    if (!mem) { fprintf(stderr, "oom\n"); return 2; }
    R[2] = STACK_BOTTOM;   // sp

    // 读汇编
    vector<Insn> code;
    unordered_map<string, uint32_t> labels;
    unordered_map<string, uint32_t> data_syms;   // 全局变量名 → 地址
    uint32_t data_off = 0;                        // .data 段游标
    bool in_data = false;
    uint32_t pending_data = 0;                    // 当前 .word 应写入的地址
    char line[512];
    while (fgets(line, sizeof line, stdin)) {
        // 去掉注释、回车、空白
        char* c = strchr(line, '#'); if (c) *c = 0;
        size_t ll = strlen(line);
        while (ll && (line[ll-1]=='\n' || line[ll-1]=='\r')) line[--ll] = 0;
        // 指令
        char op[64] = {0};
        if (sscanf(line, " %63s", op) != 1) continue;
        string ops = op;
        if (ops == ".data") { in_data = true; continue; }
        if (ops == ".text") { in_data = false; continue; }
        if (ops == ".globl") continue;
        if (ops == ".word") {                     // 全局变量初值写入内存
            int v = 0;
            sscanf(line, " %*s %d", &v);
            *(int32_t*)(mem + pending_data) = v;
            continue;
        }
        if (!ops.empty() && ops.back() == ':') {  // 标签
            string name = ops.substr(0, ops.size() - 1);
            if (in_data) {
                pending_data = GLOBALS_BASE + data_off;
                data_syms[name] = pending_data;
                data_off += 4;
            } else {
                labels[name] = (uint32_t)code.size();
            }
            continue;
        }
        Insn in; in.op = ops;
        // 解析操作数，支持 imm(rs) 形式。先找到操作码之后的部分
        char rest[512]; rest[0] = 0;
        {
            char* tokpos = strstr(line, ops.c_str());
            if (tokpos) {
                char* q = tokpos + ops.size();
                while (*q == ' ' || *q == '\t') q++;
                strcpy(rest, q);
            }
            size_t l = strlen(rest);
            while (l && (rest[l-1]=='\n'||rest[l-1]=='\r')) rest[--l] = 0;
        }

        // 按逗号分割
        vector<string> parts;
        char* tok = strtok(rest, ",");
        while (tok) {
            string t = tok;
            while (!t.empty() && t[0]==' ') t.erase(0,1);
            while (!t.empty() && t.back()==' ') t.pop_back();
            parts.push_back(t);
            tok = strtok(nullptr, ",");
        }
        if (parts.size() >= 1) in.a = parts[0];
        if (parts.size() >= 2) in.b = parts[1];
        if (parts.size() >= 3) in.c = parts[2];
        // imm(rs) 形式
        if (in.b.find('(') != string::npos) {
            // lw t0, 4(sp)  → b=4, c=sp
            size_t lp = in.b.find('(');
            in.imm = atoi(in.b.substr(0, lp).c_str());
            in.c = in.b.substr(lp+1, in.b.find(')')-lp-1);
            in.b.clear();
        }
        if (in.b.size() && (in.b[0]=='-' || isdigit((unsigned char)in.b[0])))
            { in.imm = atoi(in.b.c_str()); in.b.clear(); }
        if (in.c.size() && (in.c[0]=='-' || isdigit((unsigned char)in.c[0])))
            { in.imm = atoi(in.c.c_str()); in.c.clear(); }
        code.push_back(in);
    }

    // main 入口（无 _start，设 ra 为哨兵：main 的 ret 会触到非法地址 → 视为程序结束）
    R[1] = 0xFFFFFFFFu;
    pc = labels["main"];
    uint32_t steps = 0;
    const uint32_t MAXSTEPS = 200000000;
    while (steps++ < MAXSTEPS) {
        if (pc >= code.size()) { fprintf(stderr, "PC out of range\n"); return 3; }
        Insn& i = code[pc];
        // 找目标（j/call 的标签）
        auto findlbl = [&](const string& s) {
            auto it = labels.find(s);
            if (it == labels.end()) { fprintf(stderr, "bad label %s\n", s.c_str()); exit(4); }
            return it->second;
        };
        if (i.op == "addi") R[regno(i.a)] = r(regno(i.b)) + i.imm;
        else if (i.op == "add") R[regno(i.a)] = r(regno(i.b)) + r(regno(i.c));
        else if (i.op == "sub") R[regno(i.a)] = r(regno(i.b)) - r(regno(i.c));
        else if (i.op == "mul") R[regno(i.a)] = r(regno(i.b)) * r(regno(i.c));
        else if (i.op == "div") R[regno(i.a)] = r(regno(i.b)) / r(regno(i.c));
        else if (i.op == "rem") R[regno(i.a)] = r(regno(i.b)) % r(regno(i.c));
        else if (i.op == "slt") R[regno(i.a)] = r(regno(i.b)) < r(regno(i.c)) ? 1 : 0;
        else if (i.op == "sltu") R[regno(i.a)] = (uint32_t)r(regno(i.b)) < (uint32_t)r(regno(i.c)) ? 1 : 0;
        else if (i.op == "sltiu") R[regno(i.a)] = (uint32_t)r(regno(i.b)) < (uint32_t)i.imm ? 1 : 0;
        else if (i.op == "slti") R[regno(i.a)] = r(regno(i.b)) < i.imm ? 1 : 0;
        else if (i.op == "slli") R[regno(i.a)] = r(regno(i.b)) << i.imm;
        else if (i.op == "xori") R[regno(i.a)] = r(regno(i.b)) ^ i.imm;
        else if (i.op == "seqz") R[regno(i.a)] = r(regno(i.b)) == 0 ? 1 : 0;   // sltiu rd,rs,1
        else if (i.op == "snez") R[regno(i.a)] = r(regno(i.b)) != 0 ? 1 : 0;   // sltu rd,x0,rs
        else if (i.op == "li") R[regno(i.a)] = i.imm;
        else if (i.op == "mv") R[regno(i.a)] = r(regno(i.b));
        else if (i.op == "neg") R[regno(i.a)] = -r(regno(i.b));
        else if (i.op == "lw") { uint32_t addr = r(regno(i.c)) + i.imm; R[regno(i.a)] = *(int32_t*)(mem+addr); }
        else if (i.op == "sw") { uint32_t addr = r(regno(i.c)) + i.imm; *(int32_t*)(mem+addr) = r(regno(i.a)); }
        else if (i.op == "la") {
            auto it = data_syms.find(i.b);
            if (it == data_syms.end()) { fprintf(stderr, "bad global %s\n", i.b.c_str()); return 7; }
            R[regno(i.a)] = it->second;
        }
        else if (i.op == "beqz" || i.op == "beq") {
            int t = (i.op=="beqz") ? (r(regno(i.a))==0) : (r(regno(i.a))==r(regno(i.b)));
            if (t) { pc = findlbl(i.b); continue; }
        }
        else if (i.op == "bnez" || i.op == "bne") {
            int t = (i.op=="bnez") ? (r(regno(i.a))!=0) : (r(regno(i.a))!=r(regno(i.b)));
            if (t) { pc = findlbl(i.b); continue; }
        }
        else if (i.op == "blt") {
            if (r(regno(i.a)) < r(regno(i.b))) { pc = findlbl(i.c); continue; }
        }
        else if (i.op == "bge") {
            if (r(regno(i.a)) >= r(regno(i.b))) { pc = findlbl(i.c); continue; }
        }
        else if (i.op == "j") { pc = findlbl(i.a); continue; }
        else if (i.op == "jal") { R[regno(i.b)] = pc + 1; pc = findlbl(i.a); continue; }
        else if (i.op == "call") { R[1] = pc + 1; pc = findlbl(i.a); continue; }
        else if (i.op == "ret") {
            if (R[1] >= code.size()) {  // 返回地址非法 → main 返回
                return (int)(R[10] & 0xFF);
            }
            pc = R[1]; continue;
        }
        else if (i.op == "jalr") { uint32_t t = r(regno(i.b)) + i.imm; R[regno(i.a)] = pc + 1; pc = t; continue; }
        else { fprintf(stderr, "unknown insn: %s\n", i.op.c_str()); return 5; }
        pc++;
    }
    fprintf(stderr, "too many steps (likely infinite loop)\n");
    return 6;
}
