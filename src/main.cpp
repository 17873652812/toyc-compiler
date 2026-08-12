#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <stdexcept>
#include <cstring>
#include <cstdio>
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

    // 读输入（用 C stdio 读取，避免部分工具链的 fstream 在 -O2 下的库 bug）
    string source;
    if (filename) {
        FILE* fp = fopen(filename, "rb");
        if (!fp) { cerr << "cannot open: " << filename << endl; return 1; }
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, fp)) > 0) source.append(buf, n);
        fclose(fp);
    } else {
        char c;
        while (fread(&c, 1, 1, stdin) == 1) source.push_back(c);
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
