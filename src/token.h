#pragma once
// ↑ 防止重复引入

#include "defs.h"                  // 用我们自己定义的 Position
#include <string>                  // std::string
#include <unordered_map>           // std::unordered_map — 字典/查表（关键字→Token类型）

namespace toyc {

// ============================================================
// TokenKind — Token 的类型（这道选择题有 11 个选项）
// ============================================================
// enum class 就是定义一道选择题：
//   计算机眼里这些是 0 1 2 3… 的数字，但写代码的人用名字更直观
//   比如 TokenKind::KW_INT 比数字 0 好理解多了
enum class TokenKind {
    // ---- 关键字 ----
    KW_INT,       // int
    KW_RETURN,    // return

    // ---- 标识符 & 字面量 ----
    IDENT,        // 标识符：用户起的名字，如 main、x、fib
    NUMBER,       // 数字字面量：如 42、0、100

    // ---- 符号 ----
    LPAREN,       // (
    RPAREN,       // )
    LBRACE,       // {
    RBRACE,       // }
    SEMICOLON,    // ;

    // ---- 特殊 ----
    END,          // 文件结束标记（源码读完了）
    ERR,          // 非法字符（Lexer 不认识这个字符）
};

// ============================================================
// Token — 代表源码里的一个"单词"
// ============================================================
// struct = 把三个相关的变量打包在一起：
//   kind   — 这个单词是什么类型（是关键字？标识符？数字？）
//   lexeme — 这个单词的原始文字（"int"、"main"、"42"）
//   pos    — 这个单词在源码的第几行第几列
//
// 使用示例：
//   Token t(TokenKind::KW_INT, "int", Position{1, 1});
//   t.kind       → TokenKind::KW_INT（数字 0）
//   t.lexeme     → "int"
//   t.pos.line   → 1
//   t.pos.col    → 1
struct Token {
    TokenKind kind;        // 这个单词的类型
    std::string lexeme;    // 这个单词的原始文字
    Position pos;          // 在源码中的位置

    // ---- 构造函数：一行代码创建一个 Token ----
    // 比如写 Token(TokenKind::IDENT, "main", Position{1, 5})
    // 三个参数分别塞进 kind、lexeme、pos 三个格子里
    // std::move(lex) 是效率优化：把文字"移动"进来而不是"复制"进来
    Token(TokenKind k, std::string lex, Position p)
        : kind(k), lexeme(std::move(lex)), pos(p) {}

    // ---- kind_name()：把 Token 类型翻译成可读字符串 ----
    // 为什么需要这个？因为 kind 存的是数字（KW_INT = 0, IDENT = 2...）
    // 直接 cout << t.kind 会输出数字，没人看得懂
    // 这个函数把数字转成文字：0 → "KW_INT"，2 → "IDENT"
    // 只在调试的时候用，正式编译器输出不需要它
    std::string kind_name() const {
        switch (kind) {                          // 根据 kind 的值...
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
            default:                    return "???";  // 理论上不会到这里
        }
    }
};

// ============================================================
// keywords — 关键字字典表
// ============================================================
// Lexer 读到 "int" 时，查这个字典：
//   找到了 → 说明 "int" 是关键字，返回 KW_INT
//   没找到 → 说明不是关键字，是用户定义的标识符名字
//
// 目前 v0.1 只有 int 和 return 两个关键字
// 后面会逐渐加入 void const if else while break continue
inline const std::unordered_map<std::string, TokenKind> keywords = {
    {"int",    TokenKind::KW_INT},
    {"return", TokenKind::KW_RETURN},
};

}  // namespace toyc
