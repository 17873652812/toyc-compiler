#pragma once

#include "token.h"
#include "ast.h"
#include <stdexcept>

namespace toyc {

// Parser：语法分析器，Token 列表 → AST 语法树
//
// 递归下降解析：每种语法规则对应一个 parse 函数
// v0.1 只认一种结构：
//   CompUnit  → 'int' IDENT '(' ')' '{' 'return' NUMBER ';' '}'
class Parser {
public:
    explicit Parser(std::vector<Token> tokens)
        : tokens_(std::move(tokens)), pos_(0) {}

    // 主函数：解析整个程序
    std::unique_ptr<CompUnit> parse() {
        auto unit = std::make_unique<CompUnit>();

        while (!is_end()) {
            if (match(TokenKind::KW_INT)) {
                unit->funcs.push_back(parse_func_def());
            } else {
                error("expected 'int' (function definition)");
            }
        }

        return unit;
    }

private:
    std::vector<Token> tokens_;   // Token 列表
    size_t pos_;                  // 当前处理到第几个 Token

    // 是否到文件尾
    bool is_end() const {
        return pos_ >= tokens_.size()
            || tokens_[pos_].kind == TokenKind::END
            || tokens_[pos_].kind == TokenKind::ERR;
    }

    // 看当前 Token，不前进
    Token peek() const { return tokens_[pos_]; }

    // 吃掉当前 Token，往前走
    Token advance() { return tokens_[pos_++]; }

    // 如果当前 Token 类型匹配，吃掉它并返回 true；否则返回 false
    bool match(TokenKind k) {
        if (!is_end() && peek().kind == k) {
            advance();
            return true;
        }
        return false;
    }

    // 要求当前 Token 必须是某种类型，不然报错并退出
    Token expect(TokenKind k, const std::string& msg) {
        if (!is_end() && peek().kind == k)
            return advance();
        error(msg);
        return Token(TokenKind::ERR, "", Position{}); // never reached
    }

    // 报错：打印位置信息并抛出异常
    [[noreturn]] void error(const std::string& msg) {
        auto tok = peek();
        throw std::runtime_error(
            "Parse error at line " + std::to_string(tok.pos.line)
            + " col " + std::to_string(tok.pos.col)
            + ": " + msg
        );
    }

    // ---- 语法规则函数 ----

    // FuncDef → IDENT '(' ')' '{' ReturnStmt '}'
    // （开头的 'int' 已经在 parse() 里 match 掉了）
    std::unique_ptr<FuncDef> parse_func_def() {
        Token name_tok = expect(TokenKind::IDENT, "expected function name");
        expect(TokenKind::LPAREN, "expected '('");
        expect(TokenKind::RPAREN, "expected ')'");
        expect(TokenKind::LBRACE, "expected '{'");

        auto body = parse_return_stmt();

        expect(TokenKind::RBRACE, "expected '}'");
        return std::make_unique<FuncDef>(name_tok.lexeme, std::move(body));
    }

    // ReturnStmt → 'return' Expr ';'
    std::unique_ptr<ReturnStmt> parse_return_stmt() {
        expect(TokenKind::KW_RETURN, "expected 'return'");
        auto expr = parse_expr();
        expect(TokenKind::SEMICOLON, "expected ';'");
        return std::make_unique<ReturnStmt>(std::move(expr));
    }

    // Expr → NUMBER（v0.1 只支持数字字面量）
    std::unique_ptr<ASTNode> parse_expr() {
        Token tok = expect(TokenKind::NUMBER, "expected a number");
        int val = std::stoi(tok.lexeme);   // "42" → 42
        return std::make_unique<NumberExpr>(val);
    }
};

}  // namespace toyc
