#pragma once

#include "defs.h"
#include <string>
#include <unordered_map>

namespace toyc {

// ============================================================
// Token 类型枚举 — 源码中的每种"单词"
// ============================================================

enum class TokenKind {
    // 关键字
    KW_INT,      // int
    KW_RETURN,   // return

    // 标识符 & 字面量
    IDENT,       // 名字，如 main
    NUMBER,      // 整数，如 42

    // 符号
    LPAREN,      // (
    RPAREN,      // )
    LBRACE,      // {
    RBRACE,      // }
    SEMICOLON,   // ;

    // 特殊
    END,         // 文件结束
    ERR,         // 非法字符
};

// ============================================================
// Token — 一个"单词"：类型 + 文本 + 位置
// ============================================================

struct Token {
    TokenKind kind;      // 什么类型
    std::string lexeme;  // 原始文本
    Position pos;        // 在第几行第几列

    // 构造函数：创建一个 Token
    Token(TokenKind k, std::string lex, Position p)
        : kind(k), lexeme(std::move(lex)), pos(p) {}

    // 调试用：打印 Token 类型名
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

// ============================================================
// 关键字映射表 — "int" → KW_INT, "return" → KW_RETURN
// ============================================================

inline const std::unordered_map<std::string, TokenKind> keywords = {
    {"int",    TokenKind::KW_INT},
    {"return", TokenKind::KW_RETURN},
};

}  // namespace toyc
