#pragma once

#include "token.h"
#include <cctype>     // isalpha isdigit isalnum — 判断字符类型的工具

namespace toyc {

// ============================================================
// Lexer — 词法分析器
// ============================================================
// 把源码字符串切成 Token 序列（就像把句子切成单词）
//
// 输入：一段 C++ string（比如 "int main() { return 42; }"）
// 输出：一个 vector<Token>（Token 列表）
//
// 工作原理：从第一个字符到最后一个字符，逐个扫描
//   遇到字母 → 读完整个单词 → 查关键字字典 → 决定是关键字还是标识符
//   遇到数字 → 读完整个数字 → 创建 NUMBER Token
//   遇到符号 → 看看是哪种括号/分号 → 创建对应的 Token
//   遇到空白 → 跳过
//
// 使用示例：
//   Lexer lexer("int main() { return 42; }");
//   auto tokens = lexer.tokenize();  // 切出所有 Token
class Lexer {
public:
    // ---- 构造函数：接收源码字符串，初始化扫描位置 ----
    // source_ = 源码
    // pos_ = 0 表示从第 0 个字符开始扫描
    // line_ = 1, col_ = 1 表示从第 1 行第 1 列开始
    explicit Lexer(std::string src)
        : source_(std::move(src)), pos_(0), line_(1), col_(1) {}

    // ---- tokenize()：主函数，返回全部 Token ----
    // 循环调用 next_token() 直到文件结束或遇到错误
    std::vector<Token> tokenize() {
        std::vector<Token> tokens;           // 创建一个空列表
        while (true) {
            auto tok = next_token();         // 切下一个 Token
            TokenKind k = tok.kind;
            tokens.push_back(std::move(tok)); // 加入列表
            if (k == TokenKind::END || k == TokenKind::ERR)
                break;                       // 文件结束或出错就停
        }
        return tokens;
    }

private:
    // ---- 四个成员变量 ----
    std::string source_;   // 源码文本（一整段，不会修改）
    size_t pos_;           // 当前扫描位置（指向 source_ 的第几个字符）
    int line_;             // 当前在第几行（用于记录 Token 位置）
    int col_;              // 当前在第几列

    // ========== 辅助函数 ==========

    // ---- peek()：看一眼当前字符，不移动位置 ----
    // 就像看书时眼睛扫一眼当前字母，手指不动
    // 如果已经读到文件尾，返回 '\0'（空字符）
    char peek() const {
        if (pos_ >= source_.size()) return '\0';
        return source_[pos_];
    }

    // ---- advance()：吃掉当前字符，往前走一步 ----
    // 返回被吃掉的字符，同时更新行列号
    // 如果遇到换行符 '\n'，行号 +1，列号归 1
    char advance() {
        if (pos_ >= source_.size()) return '\0';
        char c = source_[pos_++];   // 吃掉 source_[pos_]，然后 pos_ 加 1
        if (c == '\n') { line_++; col_ = 1; }
        else { col_++; }
        return c;
    }

    // ---- current_pos()：返回当前的行列号 ----
    // 用于创建 Token 时记录位置
    Position current_pos() const {
        return Position{line_, col_};
    }

    // ---- skip_whitespace()：跳过所有空白字符 ----
    // 空格、Tab、换行、回车都跳过
    void skip_whitespace() {
        while (pos_ < source_.size()) {
            char c = peek();
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                advance();   // 是空白，跳过
            else
                break;       // 不是空白，停
        }
    }

    // ========== 核心函数 ==========

    // ---- next_token()：扫描并返回下一个 Token ----
    // 这是 Lexer 的大脑：
    //   1. 跳过空白
    //   2. 看当前字符是什么 → 决定怎么处理
    //   3. 创建并返回对应的 Token
    Token next_token() {
        // 第 1 步：跳过所有空白
        skip_whitespace();

        // 第 2 步：如果已经读到文件尾，返回 END 标记
        if (pos_ >= source_.size())
            return make(TokenKind::END, "");

        // 记录当前 Token 的起始位置（在跳空白之后、读内容之前）
        Position p = current_pos();
        char c = peek();    // 看一眼第一个字符，决定类型

        // 第 3 步：根据第一个字符判断这个 Token 是什么

        // ---- 情况 A：字母或下划线开头 → 是一个单词 ----
        // 可能是关键字（int、return），也可能是用户起的名字（main、x）
        // 读完整个单词（字母、数字、下划线都可以是单词的一部分）
        // 然后查 keywords 字典判断是关键字还是普通标识符
        if (std::isalpha(c) || c == '_') {
            std::string word;
            // 一直读，直到遇到非字母/数字/下划线
            while (std::isalnum(peek()) || peek() == '_')
                word += advance();

            // 查字典
            auto it = keywords.find(word);
            if (it != keywords.end())
                // 找到了 → 是关键字
                return make(it->second, word, p);
            else
                // 没找到 → 是普通标识符名字
                return make(TokenKind::IDENT, word, p);
        }

        // ---- 情况 B：数字开头 → 数字字面量 ----
        // 读完所有连续的数字
        if (std::isdigit(c)) {
            std::string num;
            while (std::isdigit(peek()))
                num += advance();
            return make(TokenKind::NUMBER, num, p);
        }

        // ---- 情况 C：符号 ----
        // 单字符符号：直接吃掉，返回对应类型
        advance();   // 吃掉这个字符
        switch (c) {
            case '(': return make(TokenKind::LPAREN,    "(", p);
            case ')': return make(TokenKind::RPAREN,    ")", p);
            case '{': return make(TokenKind::LBRACE,    "{", p);
            case '}': return make(TokenKind::RBRACE,    "}", p);
            case ';': return make(TokenKind::SEMICOLON, ";", p);

            // ---- 情况 D：都不匹配 → 非法字符 ----
            default:  return make(TokenKind::ERR, std::string(1, c), p);
        }
    }

    // ---- make()：快捷创建 Token 的辅助函数（两个重载版本） ----
    // 重载 = 同一个函数名，参数不同，自动选匹配的那个
    Token make(TokenKind k, std::string lex, Position p) {
        // 三个参数版：指定类型、文字、位置
        return Token(k, std::move(lex), p);
    }
    Token make(TokenKind k, std::string lex) {
        // 两个参数版：不指定位置，用当前位置
        return Token(k, std::move(lex), current_pos());
    }
};

}  // namespace toyc
