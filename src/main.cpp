// ============================================================
// main.cpp —— 编译器入口
//
// 整个编译流程：
//   源码 (.tc) → Lexer → Token 流 → Parser → AST → Codegen → RISC-V 汇编
//
// 命令行用法：
//   compiler input.tc           编译（不优化）
//   compiler -opt input.tc      编译并开启优化
//   compiler < input.tc         从标准输入读取源码
//
// 输出：RISC-V 汇编写到标准输出（stdout）。
// ============================================================
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <stdexcept>
#include <cstring>
#include "defs.h"
#include "token.h"
#include "lexer.h"
#include "parser.h"
#include "codegen.h"
using namespace std;
using namespace toyc;

int main(int argc, char* argv[]) {
    // 解析 -opt 参数（v1.0）
    bool opt = false;
    const char* filename = nullptr;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-opt") == 0) opt = true;
        else filename = argv[i];
    }

    // 读输入
    string source;
    if (filename) {
        ifstream file(filename);
        if (!file) { cerr << "cannot open: " << filename << endl; return 1; }
        ostringstream oss; oss << file.rdbuf(); source = oss.str();
    } else {
        string line;
        while (getline(cin, line)) source += line + '\n';
    }
    if (source.empty()) { cerr << "empty input" << endl; return 1; }

    try {
        Lexer lexer(source);
        auto tokens = lexer.tokenize();
        Parser parser(move(tokens));
        auto ast = parser.parse();
        Codegen codegen(*ast, opt);
        cout << codegen.generate();
    } catch (const exception& e) {
        cerr << "Error: " << e.what() << endl;
        return 1;
    }
    return 0;
}
