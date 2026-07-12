#pragma once

#include "defs.h"
#include <string>
#include <unordered_map>

namespace toyc {

// Token 的类型
enum class TokenKind {
    // 关键字
    KW_INT,      // int
    KW_RETURN,   // return

    // 标识符 & 字面量
    IDENT,       // 名字
    NUMBER,      // 数字

    // 运算符（v0.2 新增）
    PLUS,        // +
    MINUS,       // -
    STAR,        // *
    SLASH,       // /
    PERCENT,     // %
    ASSIGN,      // =
    NOT,         // !
    COMMA,       // ,

    // 界符
    LPAREN,      // (
    RPAREN,      // )
    LBRACE,      // {
    RBRACE,      // }
    SEMICOLON,   // ;

    // 特殊
    END,         // 文件结束
    ERR,         // 非法字符
};

struct Token {
    TokenKind kind;
    std::string lexeme;
    Position pos;

    Token(TokenKind k, std::string lex, Position p)
        : kind(k), lexeme(std::move(lex)), pos(p) {}

    // 调试用：类型转文字
    std::string kind_name() const {
        switch (kind) {
            case TokenKind::KW_INT:     return "KW_INT";
            case TokenKind::KW_RETURN:  return "KW_RETURN";
            case TokenKind::IDENT:      return "IDENT";
            case TokenKind::NUMBER:     return "NUMBER";
            case TokenKind::PLUS:       return "PLUS";
            case TokenKind::MINUS:      return "MINUS";
            case TokenKind::STAR:       return "STAR";
            case TokenKind::SLASH:      return "SLASH";
            case TokenKind::PERCENT:    return "PERCENT";
            case TokenKind::ASSIGN:     return "ASSIGN";
            case TokenKind::NOT:        return "NOT";
            case TokenKind::COMMA:      return "COMMA";
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

// 关键字字典
inline const std::unordered_map<std::string, TokenKind> keywords = {
    {"int",    TokenKind::KW_INT},
    {"return", TokenKind::KW_RETURN},
};

}  // namespace toyc
