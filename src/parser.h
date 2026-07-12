#pragma once

#include "token.h"
#include "ast.h"
#include <stdexcept>

namespace toyc {

// Parser：递归下降语法分析器，Token → AST
//
// 语法规则（v0.2）：
//   CompUnit  → FuncDef*
//   FuncDef   → 'int' IDENT '(' ')' Block
//   Block     → '{' Stmt* '}'
//   Stmt      → VarDecl | AssignStmt | ReturnStmt
//   VarDecl   → 'int' IDENT '=' Expr ';'
//   AssignStmt→ IDENT '=' Expr ';'
//   ReturnStmt→ 'return' Expr ';'
//   Expr      → AddExpr
//   AddExpr   → MulExpr (('+'|'-') MulExpr)*
//   MulExpr   → UnaryExpr (('*'|'/'|'%') UnaryExpr)*
//   UnaryExpr → ('+'|'-'|'!') UnaryExpr | PrimaryExpr
//   PrimaryExpr→ IDENT | NUMBER | '(' Expr ')' | CallExpr
//   CallExpr  → IDENT '(' (Expr (',' Expr)*)? ')'（v0.5）
class Parser {
public:
    explicit Parser(std::vector<Token> tokens)
        : tokens_(std::move(tokens)), pos_(0) {}

    std::unique_ptr<CompUnit> parse() {
        auto unit = std::make_unique<CompUnit>();
        while (!is_end()) {
            if (check(TokenKind::KW_INT) || check(TokenKind::KW_VOID)) {
                unit->funcs.push_back(parse_func_def());
            } else {
                error("expected 'int' or 'void'");
            }
        }
        return unit;
    }

private:
    std::vector<Token> tokens_;
    size_t pos_;

    bool is_end() const {
        return pos_ >= tokens_.size()
            || tokens_[pos_].kind == TokenKind::END
            || tokens_[pos_].kind == TokenKind::ERR;
    }

    Token peek() const { return tokens_[pos_]; }
    Token advance() { return tokens_[pos_++]; }

    bool match(TokenKind k) {
        if (!is_end() && peek().kind == k) {
            advance();
            return true;
        }
        return false;
    }

    // 检查下一个 Token 是否是指定类型（不消耗）
    bool check(TokenKind k) const {
        return !is_end() && peek().kind == k;
    }

    Token expect(TokenKind k, const std::string& msg) {
        if (!is_end() && peek().kind == k)
            return advance();
        error(msg);
        return Token(TokenKind::ERR, "", Position{});
    }

    [[noreturn]] void error(const std::string& msg) {
        auto tok = peek();
        throw std::runtime_error(
            "Parse error at line " + std::to_string(tok.pos.line)
            + " col " + std::to_string(tok.pos.col)
            + ": " + msg
        );
    }

    // ---- 语法规则 ----

    // FuncDef → ('int'|'void') IDENT '(' (Param (',' Param)*)? ')' Block（v0.5）
    // Param → 'int' IDENT
    std::unique_ptr<FuncDef> parse_func_def() {
        // 返回类型
        std::string ret_type;
        if (match(TokenKind::KW_INT)) ret_type = "int";
        else if (match(TokenKind::KW_VOID)) ret_type = "void";
        else error("expected 'int' or 'void'");

        Token name_tok = expect(TokenKind::IDENT, "expected function name");
        expect(TokenKind::LPAREN, "expected '('");

        // 参数列表（每个参数都是 int 类型）
        std::vector<std::string> params;
        if (!check(TokenKind::RPAREN)) {
            expect(TokenKind::KW_INT, "expected 'int'");
            Token p = expect(TokenKind::IDENT, "expected parameter name");
            params.push_back(p.lexeme);
            while (match(TokenKind::COMMA)) {
                expect(TokenKind::KW_INT, "expected 'int'");
                Token p2 = expect(TokenKind::IDENT, "expected parameter name");
                params.push_back(p2.lexeme);
            }
        }
        expect(TokenKind::RPAREN, "expected ')'");
        auto body = parse_block();
        return std::make_unique<FuncDef>(ret_type, name_tok.lexeme, params, std::move(body));
    }

    // Block → '{' Stmt* '}'
    std::unique_ptr<Block> parse_block() {
        expect(TokenKind::LBRACE, "expected '{'");
        auto block = std::make_unique<Block>();
        while (!is_end() && peek().kind != TokenKind::RBRACE) {
            block->stmts.push_back(parse_stmt());
        }
        expect(TokenKind::RBRACE, "expected '}'");
        return block;
    }

    // Stmt → ... | WhileStmt | BreakStmt | ContinueStmt | Block
    std::unique_ptr<ASTNode> parse_stmt() {
        if (match(TokenKind::KW_INT)) {
            return parse_var_decl();
        }
        if (match(TokenKind::KW_RETURN)) {
            if (match(TokenKind::SEMICOLON))
                return std::make_unique<ReturnStmt>();  // void return
            auto expr = parse_expr();
            expect(TokenKind::SEMICOLON, "expected ';'");
            return std::make_unique<ReturnStmt>(std::move(expr));
        }
        if (match(TokenKind::KW_IF)) {
            return parse_if_stmt();
        }
        if (match(TokenKind::KW_WHILE)) {
            return parse_while_stmt();
        }
        if (match(TokenKind::KW_BREAK)) {
            expect(TokenKind::SEMICOLON, "expected ';'");
            return std::make_unique<BreakStmt>();
        }
        if (match(TokenKind::KW_CONTINUE)) {
            expect(TokenKind::SEMICOLON, "expected ';'");
            return std::make_unique<ContinueStmt>();
        }
        if (check(TokenKind::LBRACE)) {
            return parse_block();
        }
        if (check(TokenKind::IDENT)) {
            return parse_assign_or_call_stmt();
        }
        error("expected statement");
        return nullptr;
    }

    // AssignStmt → IDENT '=' Expr ';'  或  CallExpr ';'（v0.5）
    std::unique_ptr<ASTNode> parse_assign_or_call_stmt() {
        Token name_tok = advance();  // 吃掉 IDENT
        if (match(TokenKind::ASSIGN)) {
            auto expr = parse_expr();
            expect(TokenKind::SEMICOLON, "expected ';'");
            return std::make_unique<AssignStmt>(name_tok.lexeme, std::move(expr));
        }
        if (check(TokenKind::LPAREN)) {
            // 函数调用作为语句：f(args);
            // 回退 IDENT，用 parse_primary_expr 里的函数调用逻辑
            pos_--;
            auto expr = parse_expr();
            expect(TokenKind::SEMICOLON, "expected ';'");
            return expr;  // CallExpr 直接当语句
        }
        error("expected '=' or '('");
        return nullptr;
    }

    // VarDecl → 'int' IDENT '=' Expr ';'
    // （'int' 已在 parse_stmt 里 match 掉了）
    std::unique_ptr<VarDecl> parse_var_decl() {
        Token name_tok = expect(TokenKind::IDENT, "expected variable name");
        expect(TokenKind::ASSIGN, "expected '='");
        auto init = parse_expr();
        expect(TokenKind::SEMICOLON, "expected ';'");
        return std::make_unique<VarDecl>(name_tok.lexeme, std::move(init));
    }

    // AssignStmt → IDENT '=' Expr ';'
    std::unique_ptr<AssignStmt> parse_assign_stmt() {
        Token name_tok = advance();  // 已经 check 过是 IDENT
        expect(TokenKind::ASSIGN, "expected '='");
        auto expr = parse_expr();
        expect(TokenKind::SEMICOLON, "expected ';'");
        return std::make_unique<AssignStmt>(name_tok.lexeme, std::move(expr));
    }

    // IfStmt → 'if' '(' Expr ')' Stmt ('else' Stmt)?
    std::unique_ptr<IfStmt> parse_if_stmt() {
        expect(TokenKind::LPAREN, "expected '(' after 'if'");
        auto cond = parse_expr();
        expect(TokenKind::RPAREN, "expected ')'");
        auto then_stmt = parse_stmt();
        std::unique_ptr<ASTNode> else_stmt;
        if (match(TokenKind::KW_ELSE)) {
            else_stmt = parse_stmt();
        }
        return std::make_unique<IfStmt>(std::move(cond),
            std::move(then_stmt), std::move(else_stmt));
    }

    // WhileStmt → 'while' '(' Expr ')' Stmt（v0.4）
    // （'while' 已在 parse_stmt 里 match 掉了）
    std::unique_ptr<WhileStmt> parse_while_stmt() {
        expect(TokenKind::LPAREN, "expected '(' after 'while'");
        auto cond = parse_expr();
        expect(TokenKind::RPAREN, "expected ')'");
        auto body = parse_stmt();
        return std::make_unique<WhileStmt>(std::move(cond), std::move(body));
    }

    // ---- 表达式（按优先级分层） ----

    // Expr → RelExpr
    std::unique_ptr<ASTNode> parse_expr() {
        return parse_rel_expr();
    }

    // RelExpr → AddExpr (('<'|'>'|'<='|'>='|'=='|'!=') AddExpr)*（v0.3）
    std::unique_ptr<ASTNode> parse_rel_expr() {
        auto left = parse_add_expr();
        while (check(TokenKind::LT) || check(TokenKind::GT)
               || check(TokenKind::LE) || check(TokenKind::GE)
               || check(TokenKind::EQ) || check(TokenKind::NE)) {
            std::string op = advance().lexeme;
            auto right = parse_add_expr();
            left = std::make_unique<BinaryExpr>(
                std::move(left), op, std::move(right));
        }
        return left;
    }

    // AddExpr → MulExpr (('+'|'-') MulExpr)*
    std::unique_ptr<ASTNode> parse_add_expr() {
        auto left = parse_mul_expr();
        while (check(TokenKind::PLUS) || check(TokenKind::MINUS)) {
            std::string op = advance().lexeme;
            auto right = parse_mul_expr();
            left = std::make_unique<BinaryExpr>(
                std::move(left), op, std::move(right));
        }
        return left;
    }

    // MulExpr → UnaryExpr (('*'|'/'|'%') UnaryExpr)*
    std::unique_ptr<ASTNode> parse_mul_expr() {
        auto left = parse_unary_expr();
        while (check(TokenKind::STAR) || check(TokenKind::SLASH)
               || check(TokenKind::PERCENT)) {
            std::string op = advance().lexeme;
            auto right = parse_unary_expr();
            left = std::make_unique<BinaryExpr>(
                std::move(left), op, std::move(right));
        }
        return left;
    }

    // UnaryExpr → ('+'|'-'|'!') UnaryExpr | PrimaryExpr
    std::unique_ptr<ASTNode> parse_unary_expr() {
        if (check(TokenKind::MINUS) || check(TokenKind::PLUS)
            || check(TokenKind::NOT)) {
            std::string op = advance().lexeme;
            auto expr = parse_unary_expr();
            return std::make_unique<UnaryExpr>(std::move(op), std::move(expr));
        }
        return parse_primary_expr();
    }

    // PrimaryExpr → IDENT ('(' args ')')? | NUMBER | '(' Expr ')'（v0.5：函数调用）
    std::unique_ptr<ASTNode> parse_primary_expr() {
        if (match(TokenKind::NUMBER)) {
            int val = std::stoi(tokens_[pos_ - 1].lexeme);
            return std::make_unique<NumberExpr>(val);
        }
        if (match(TokenKind::IDENT)) {
            std::string name = tokens_[pos_ - 1].lexeme;
            // 如果后面跟 '('，就是函数调用
            if (match(TokenKind::LPAREN)) {
                std::vector<std::unique_ptr<ASTNode>> args;
                if (!check(TokenKind::RPAREN)) {
                    args.push_back(parse_expr());
                    while (match(TokenKind::COMMA)) {
                        args.push_back(parse_expr());
                    }
                }
                expect(TokenKind::RPAREN, "expected ')'");
                return std::make_unique<CallExpr>(name, std::move(args));
            }
            return std::make_unique<IdExpr>(name);
        }
        if (match(TokenKind::LPAREN)) {
            auto expr = parse_expr();
            expect(TokenKind::RPAREN, "expected ')'");
            return expr;
        }
        error("expected expression");
        return nullptr;
    }
};

}  // namespace toyc
