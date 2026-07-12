#include <iostream>
#include <string>
#include <stdexcept>
#include "defs.h"
#include "token.h"
#include "lexer.h"
#include "parser.h"
#include "codegen.h"
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

    try {
        // 2. 词法分析：源码 → Token 列表
        Lexer lexer(source);
        auto tokens = lexer.tokenize();

        // 3. 语法分析：Token → AST 树
        Parser parser(std::move(tokens));
        auto ast = parser.parse();

        // 4. 代码生成：AST → RISC-V 汇编
        Codegen codegen(*ast);
        cout << codegen.generate();

    } catch (const exception& e) {
        cerr << "Error: " << e.what() << endl;
        return 1;
    }

    return 0;
}
