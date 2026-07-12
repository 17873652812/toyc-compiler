#pragma once

#include "defs.h"
#include <string>
#include <unordered_map>

namespace toyc {

enum class TokenKind {
    // 关键字
    KW_INT,      KW_RETURN,   KW_IF,       KW_ELSE,
    KW_WHILE,    KW_BREAK,    KW_CONTINUE,  // v0.4

    // 标识符 & 字面量
    IDENT,       NUMBER,

    // 运算符
    PLUS, MINUS, STAR, SLASH, PERCENT, ASSIGN, NOT, COMMA,

    // 比较运算符
    LT, GT, LE, GE, EQ, NE,

    // 界符
    LPAREN, RPAREN, LBRACE, RBRACE, SEMICOLON,

    END, ERR,
};

struct Token {
    TokenKind kind;
    std::string lexeme;
    Position pos;

    Token(TokenKind k, std::string lex, Position p)
        : kind(k), lexeme(std::move(lex)), pos(p) {}

    std::string kind_name() const {
        switch (kind) {
            case TokenKind::KW_INT:      return "KW_INT";
            case TokenKind::KW_RETURN:   return "KW_RETURN";
            case TokenKind::KW_IF:       return "KW_IF";
            case TokenKind::KW_ELSE:     return "KW_ELSE";
            case TokenKind::KW_WHILE:    return "KW_WHILE";
            case TokenKind::KW_BREAK:    return "KW_BREAK";
            case TokenKind::KW_CONTINUE: return "KW_CONTINUE";
            case TokenKind::IDENT:       return "IDENT";
            case TokenKind::NUMBER:      return "NUMBER";
            case TokenKind::PLUS:        return "PLUS";
            case TokenKind::MINUS:       return "MINUS";
            case TokenKind::STAR:        return "STAR";
            case TokenKind::SLASH:       return "SLASH";
            case TokenKind::PERCENT:     return "PERCENT";
            case TokenKind::ASSIGN:      return "ASSIGN";
            case TokenKind::NOT:         return "NOT";
            case TokenKind::COMMA:       return "COMMA";
            case TokenKind::LT:          return "LT";
            case TokenKind::GT:          return "GT";
            case TokenKind::LE:          return "LE";
            case TokenKind::GE:          return "GE";
            case TokenKind::EQ:          return "EQ";
            case TokenKind::NE:          return "NE";
            case TokenKind::LPAREN:      return "LPAREN";
            case TokenKind::RPAREN:      return "RPAREN";
            case TokenKind::LBRACE:      return "LBRACE";
            case TokenKind::RBRACE:      return "RBRACE";
            case TokenKind::SEMICOLON:   return "SEMICOLON";
            case TokenKind::END:         return "END";
            case TokenKind::ERR:         return "ERR";
            default:                     return "???";
        }
    }
};

inline const std::unordered_map<std::string, TokenKind> keywords = {
    {"int",      TokenKind::KW_INT},
    {"return",   TokenKind::KW_RETURN},
    {"if",       TokenKind::KW_IF},
    {"else",     TokenKind::KW_ELSE},
    {"while",    TokenKind::KW_WHILE},
    {"break",    TokenKind::KW_BREAK},
    {"continue", TokenKind::KW_CONTINUE},
};

}  // namespace toyc
