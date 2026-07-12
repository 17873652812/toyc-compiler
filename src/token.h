#pragma once

#include "defs.h"
#include <string>
#include <unordered_map>

namespace toyc {

// Token 的类型（计算机里存数字，写代码用名字）
enum class TokenKind {
    KW_INT,      // int
    KW_RETURN,   // return
    IDENT,       // 标识符（用户起的名字）
    NUMBER,      // 数字字面量
    LPAREN,      // (
    RPAREN,      // )
    LBRACE,      // {
    RBRACE,      // }
    SEMICOLON,   // ;
    END,         // 文件结束
    ERR,         // 非法字符
};

// Token：源码里的一个"单词"（类型 + 文字 + 位置）
struct Token {
    TokenKind kind;
    std::string lexeme;
    Position pos;

    // 构造函数：一行创建 Token
    Token(TokenKind k, std::string lex, Position p)
        : kind(k), lexeme(std::move(lex)), pos(p) {}

    // 调试用：把 Token 类型从数字翻译成文字（0→"KW_INT"，2→"IDENT"...）
    std::string kind_name() const {
        switch (kind) {
            case TokenKind::KW_INT:     return "KW_INT";
            case TokenKind::KW_RETURN:  return "KW_RETURN";
            case TokenKind::IDENT:      return "IDENT";
            case TokenKind::NUMBER:     return "NUMBER";
            case TokenKind::LPAREN:     return "LPAREN";
            case TokenKind::RPAREN:     return "RPAREN";
            case TokenKind::LBRACE:     return "LBRACE";
            case TokenKind::RBRACE:     return "RBRACE";
            case TokenKind::SEMICOLON:  return "SEMICOLON";
            case TokenKind::END:        return "END";
            case TokenKind::ERR:        return "ERR";
            default:                    return "???";
        }
    }
};

// 关键字字典（"int"→KW_INT, "return"→KW_RETURN），Lexer 用
inline const std::unordered_map<std::string, TokenKind> keywords = {
    {"int",    TokenKind::KW_INT},
    {"return", TokenKind::KW_RETURN},
};

}  // namespace toyc
