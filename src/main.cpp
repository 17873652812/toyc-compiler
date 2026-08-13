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
