#include <iostream>    // cin / cout — 输入和输出
#include <string>      // string — 字符串类型
#include "defs.h"      // 我们的公共定义（namespace toyc, Position）
#include "token.h"     // Token 类型 + Token 结构体
#include "lexer.h"     // Lexer — 词法分析器（源码 → Token）
using namespace std;
using namespace toyc;

int main() {
    // ========== 第 1 步：读取全部输入 ==========
    // getline(cin, line) 从键盘/管道读一行文字
    // 返回 false 时表示读完了
    // source += line + '\n' 把每行拼起来，中间加换行
    string source;
    string line;
    while (getline(cin, line)) {
        source += line + '\n';
    }

    // 没输入 → 报错退出
    if (source.empty()) {
        cerr << "empty input" << endl;
        return 1;
    }

    // ========== 第 2 步：词法分析 ==========
    // 创建 Lexer，把源码传进去
    // tokenize() 开始扫描，返回所有 Token
    Lexer lexer(source);
    auto tokens = lexer.tokenize();

    // ========== 第 3 步：打印 Token 列表（调试用） ==========
    // 遍历 tokens 列表，逐个打印类型、文字、位置
    for (const auto& tok : tokens) {
        cout << tok.kind_name()          // Token 类型（如 "KW_INT"）
             << " '" << tok.lexeme << "'" // 原始文字（如 "int"）
             << "  at " << tok.pos.line << ":" << tok.pos.col  // 位置
             << endl;
    }

    // TODO: 第 4 步 — Parser（语法分析）
    // TODO: 第 5 步 — Codegen（代码生成）

    return 0;
}
