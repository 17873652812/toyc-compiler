#include <iostream>
#include <string>
#include <stdexcept>
#include "defs.h"
#include "token.h"
#include "lexer.h"
#include "parser.h"
using namespace std;
using namespace toyc;

// 辅助函数：打印 AST 树（递归缩进）
void print_ast(const ASTNode* node, int indent = 0) {
    string pad(indent, ' ');   // 缩进空格

    // 判断节点类型并打印（dynamic_cast = 尝试把 ASTNode* 转成具体类型）
    if (auto* n = dynamic_cast<const NumberExpr*>(node)) {
        cout << pad << "NumberExpr(" << n->value << ")" << endl;
    }
    else if (auto* r = dynamic_cast<const ReturnStmt*>(node)) {
        cout << pad << "ReturnStmt" << endl;
        print_ast(r->expr.get(), indent + 2);
    }
    else if (auto* f = dynamic_cast<const FuncDef*>(node)) {
        cout << pad << "FuncDef: " << f->name << endl;
        print_ast(f->body.get(), indent + 2);
    }
    else if (auto* c = dynamic_cast<const CompUnit*>(node)) {
        cout << pad << "CompUnit" << endl;
        for (const auto& func : c->funcs) {
            print_ast(func.get(), indent + 2);
        }
    }
}

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

        // 3. 语法分析：Token 列表 → AST 语法树
        Parser parser(std::move(tokens));
        auto ast = parser.parse();

        // 4. 打印 AST 树
        print_ast(ast.get());

    } catch (const exception& e) {
        cerr << "Error: " << e.what() << endl;
        return 1;
    }

    return 0;
}
