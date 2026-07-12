#include <iostream>
#include <string>
#include "defs.h"
#include "token.h"
#include "lexer.h"
using namespace std;
using namespace toyc;

int main() {
    // 1. 读取全部输入
    string source;
    string line;
    while (getline(cin, line)) {
        source += line + '\n';
    }

    if (source.empty()) {
        cerr << "empty input" << endl;
        return 1;
    }

    // 2. 词法分析：源码 → Token 列表
    Lexer lexer(source);
    auto tokens = lexer.tokenize();

    // 3. 打印 Token 列表（调试用，后面换成 Parser + Codegen）
    for (const auto& tok : tokens) {
        cout << tok.kind_name()
             << " '" << tok.lexeme << "'"
             << "  at " << tok.pos.line << ":" << tok.pos.col
             << endl;
    }

    return 0;
}
