#pragma once

#include "token.h"
#include <cctype>

namespace toyc {

// Lexer：词法分析器，把源码字符串切成 Token 列表
class Lexer {
public:
    explicit Lexer(std::string src)
        : source_(std::move(src)), pos_(0), line_(1), col_(1) {}

    // 主函数：扫描并返回全部 Token
    std::vector<Token> tokenize() {
        std::vector<Token> tokens;
        while (true) {
            auto tok = next_token();
            TokenKind k = tok.kind;
            tokens.push_back(std::move(tok));
            if (k == TokenKind::END || k == TokenKind::ERR)
                break;
        }
        return tokens;
    }

private:
    std::string source_;   // 源码
    size_t pos_;           // 当前扫描到第几个字符
    int line_, col_;       // 当前行号、列号

    // 看一眼当前字符，不往前走
    char peek() const {
        if (pos_ >= source_.size()) return '\0';
        return source_[pos_];
    }

    // 吃掉当前字符，往前走一步，更新行列号
    char advance() {
        if (pos_ >= source_.size()) return '\0';
        char c = source_[pos_++];
        if (c == '\n') { line_++; col_ = 1; }
        else { col_++; }
        return c;
    }

    // 获取当前行列号
    Position current_pos() const { return Position{line_, col_}; }

    // 跳过空格、Tab、换行
    void skip_whitespace() {
        while (pos_ < source_.size()) {
            char c = peek();
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                advance();
            else
                break;
        }
    }

    // 扫描下一个 Token（核心逻辑）
    Token next_token() {
        skip_whitespace();

        if (pos_ >= source_.size())
            return make(TokenKind::END, "");

        Position p = current_pos();
        char c = peek();

        // 字母/下划线 → 单词（查字典判断是关键字还是标识符）
        if (std::isalpha(c) || c == '_') {
            std::string word;
            while (std::isalnum(peek()) || peek() == '_')
                word += advance();
            auto it = keywords.find(word);
            if (it != keywords.end())
                return make(it->second, word, p);     // 关键字
            else
                return make(TokenKind::IDENT, word, p); // 标识符
        }

        // 数字 → NUMBER
        if (std::isdigit(c)) {
            std::string num;
            while (std::isdigit(peek()))
                num += advance();
            return make(TokenKind::NUMBER, num, p);
        }

        // 符号
        advance();
        switch (c) {
            case '(': return make(TokenKind::LPAREN, "(", p);
            case ')': return make(TokenKind::RPAREN, ")", p);
            case '{': return make(TokenKind::LBRACE, "{", p);
            case '}': return make(TokenKind::RBRACE, "}", p);
            case ';': return make(TokenKind::SEMICOLON, ";", p);
            // v0.2 新增运算符
            case '+': return make(TokenKind::PLUS, "+", p);
            case '-': return make(TokenKind::MINUS, "-", p);
            case '*': return make(TokenKind::STAR, "*", p);
            case '/': return make(TokenKind::SLASH, "/", p);
            case '%': return make(TokenKind::PERCENT, "%", p);
            case '=': return make(TokenKind::ASSIGN, "=", p);
            case '!': return make(TokenKind::NOT, "!", p);
            case ',': return make(TokenKind::COMMA, ",", p);
            default:  return make(TokenKind::ERR, std::string(1, c), p);
        }
    }

    // 快捷创建 Token（两个重载：有无位置参数）
    Token make(TokenKind k, std::string lex, Position p) {
        return Token(k, std::move(lex), p);
    }
    Token make(TokenKind k, std::string lex) {
        return Token(k, std::move(lex), current_pos());
    }
};

}  // namespace toyc
