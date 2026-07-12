#pragma once

#include "token.h"
#include "ast.h"
#include <stdexcept>

namespace toyc {

// Parser：递归下降语法分析器，Token → AST
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
        if (!is_end() && peek().kind == k) { advance(); return true; }
        return false;
    }
    bool check(TokenKind k) const {
        return !is_end() && peek().kind == k;
    }
    Token expect(TokenKind k, const std::string& msg) {
        if (!is_end() && peek().kind == k) return advance();
        error(msg);
        return Token(TokenKind::ERR, "", Position{});
    }
    [[noreturn]] void error(const std::string& msg) {
        auto tok = peek();
        throw std::runtime_error(
            "Parse error at line " + std::to_string(tok.pos.line)
            + " col " + std::to_string(tok.pos.col) + ": " + msg);
    }

    // ---- 函数 ----

    std::unique_ptr<FuncDef> parse_func_def() {
        std::string ret_type;
        if (match(TokenKind::KW_INT)) ret_type = "int";
        else if (match(TokenKind::KW_VOID)) ret_type = "void";
        else error("expected 'int' or 'void'");

        Token name_tok = expect(TokenKind::IDENT, "expected function name");
        expect(TokenKind::LPAREN, "expected '('");
        std::vector<std::string> params;
        if (!check(TokenKind::RPAREN)) {
            expect(TokenKind::KW_INT, "expected 'int'");
            params.push_back(expect(TokenKind::IDENT, "expected parameter name").lexeme);
            while (match(TokenKind::COMMA)) {
                expect(TokenKind::KW_INT, "expected 'int'");
                params.push_back(expect(TokenKind::IDENT, "expected parameter name").lexeme);
            }
        }
        expect(TokenKind::RPAREN, "expected ')'");
        auto body = parse_block();
        return std::make_unique<FuncDef>(ret_type, name_tok.lexeme, params, std::move(body));
    }

    // ---- Block ----

    std::unique_ptr<Block> parse_block() {
        expect(TokenKind::LBRACE, "expected '{'");
        auto block = std::make_unique<Block>();
        while (!is_end() && peek().kind != TokenKind::RBRACE)
            block->stmts.push_back(parse_stmt());
        expect(TokenKind::RBRACE, "expected '}'");
        return block;
    }

    // ---- 语句 ----

    std::unique_ptr<ASTNode> parse_stmt() {
        // const int x = expr;（v1.0）
        if (match(TokenKind::KW_CONST)) {
            return parse_const_decl();
        }
        if (match(TokenKind::KW_INT)) {
            return parse_var_decl();
        }
        if (match(TokenKind::KW_RETURN)) {
            if (match(TokenKind::SEMICOLON))
                return std::make_unique<ReturnStmt>();
            auto expr = parse_expr();
            expect(TokenKind::SEMICOLON, "expected ';'");
            return std::make_unique<ReturnStmt>(std::move(expr));
        }
        if (match(TokenKind::KW_IF)) return parse_if_stmt();
        if (match(TokenKind::KW_WHILE)) return parse_while_stmt();
        if (match(TokenKind::KW_BREAK)) {
            expect(TokenKind::SEMICOLON, "expected ';'");
            return std::make_unique<BreakStmt>();
        }
        if (match(TokenKind::KW_CONTINUE)) {
            expect(TokenKind::SEMICOLON, "expected ';'");
            return std::make_unique<ContinueStmt>();
        }
        if (check(TokenKind::LBRACE)) return parse_block();
        if (check(TokenKind::IDENT)) return parse_assign_or_call_stmt();
        error("expected statement");
        return nullptr;
    }

    std::unique_ptr<ASTNode> parse_assign_or_call_stmt() {
        Token name_tok = advance();
        if (match(TokenKind::ASSIGN)) {
            auto expr = parse_expr();
            expect(TokenKind::SEMICOLON, "expected ';'");
            return std::make_unique<AssignStmt>(name_tok.lexeme, std::move(expr));
        }
        if (check(TokenKind::LPAREN)) {
            pos_--;
            auto expr = parse_expr();
            expect(TokenKind::SEMICOLON, "expected ';'");
            return expr;
        }
        error("expected '=' or '('");
        return nullptr;
    }

    std::unique_ptr<VarDecl> parse_var_decl() {
        Token name_tok = expect(TokenKind::IDENT, "expected variable name");
        expect(TokenKind::ASSIGN, "expected '='");
        auto init = parse_expr();
        expect(TokenKind::SEMICOLON, "expected ';'");
        return std::make_unique<VarDecl>(name_tok.lexeme, std::move(init));
    }

    // ConstDecl → 'const' 'int' IDENT '=' Expr ';'（v1.0）
    // （'const' 已在 parse_stmt 里 match 掉了）
    std::unique_ptr<ConstDecl> parse_const_decl() {
        expect(TokenKind::KW_INT, "expected 'int' after 'const'");
        Token name_tok = expect(TokenKind::IDENT, "expected constant name");
        expect(TokenKind::ASSIGN, "expected '='");
        auto init = parse_expr();
        expect(TokenKind::SEMICOLON, "expected ';'");
        return std::make_unique<ConstDecl>(name_tok.lexeme, std::move(init));
    }

    std::unique_ptr<IfStmt> parse_if_stmt() {
        expect(TokenKind::LPAREN, "expected '(' after 'if'");
        auto cond = parse_expr();
        expect(TokenKind::RPAREN, "expected ')'");
        auto then_stmt = parse_stmt();
        std::unique_ptr<ASTNode> else_stmt;
        if (match(TokenKind::KW_ELSE)) else_stmt = parse_stmt();
        return std::make_unique<IfStmt>(std::move(cond), std::move(then_stmt), std::move(else_stmt));
    }

    std::unique_ptr<WhileStmt> parse_while_stmt() {
        expect(TokenKind::LPAREN, "expected '(' after 'while'");
        auto cond = parse_expr();
        expect(TokenKind::RPAREN, "expected ')'");
        auto body = parse_stmt();
        return std::make_unique<WhileStmt>(std::move(cond), std::move(body));
    }

    // ---- 表达式（按优先级从低到高） ----

    // Expr → LOrExpr（v1.0）
    std::unique_ptr<ASTNode> parse_expr() { return parse_lor_expr(); }

    // LOrExpr → LAndExpr ('||' LAndExpr)*（v1.0）
    std::unique_ptr<ASTNode> parse_lor_expr() {
        auto left = parse_land_expr();
        while (match(TokenKind::OR)) {
            auto right = parse_land_expr();
            left = std::make_unique<BinaryExpr>(std::move(left), "||", std::move(right));
        }
        return left;
    }

    // LAndExpr → RelExpr ('&&' RelExpr)*（v1.0）
    std::unique_ptr<ASTNode> parse_land_expr() {
        auto left = parse_rel_expr();
        while (match(TokenKind::AND)) {
            auto right = parse_rel_expr();
            left = std::make_unique<BinaryExpr>(std::move(left), "&&", std::move(right));
        }
        return left;
    }

    std::unique_ptr<ASTNode> parse_rel_expr() {
        auto left = parse_add_expr();
        while (check(TokenKind::LT) || check(TokenKind::GT)
               || check(TokenKind::LE) || check(TokenKind::GE)
               || check(TokenKind::EQ) || check(TokenKind::NE)) {
            std::string op = advance().lexeme;
            auto right = parse_add_expr();
            left = std::make_unique<BinaryExpr>(std::move(left), op, std::move(right));
        }
        return left;
    }

    std::unique_ptr<ASTNode> parse_add_expr() {
        auto left = parse_mul_expr();
        while (check(TokenKind::PLUS) || check(TokenKind::MINUS)) {
            std::string op = advance().lexeme;
            auto right = parse_mul_expr();
            left = std::make_unique<BinaryExpr>(std::move(left), op, std::move(right));
        }
        return left;
    }

    std::unique_ptr<ASTNode> parse_mul_expr() {
        auto left = parse_unary_expr();
        while (check(TokenKind::STAR) || check(TokenKind::SLASH) || check(TokenKind::PERCENT)) {
            std::string op = advance().lexeme;
            auto right = parse_unary_expr();
            left = std::make_unique<BinaryExpr>(std::move(left), op, std::move(right));
        }
        return left;
    }

    std::unique_ptr<ASTNode> parse_unary_expr() {
        if (check(TokenKind::MINUS) || check(TokenKind::PLUS) || check(TokenKind::NOT)) {
            std::string op = advance().lexeme;
            auto expr = parse_unary_expr();
            return std::make_unique<UnaryExpr>(std::move(op), std::move(expr));
        }
        return parse_primary_expr();
    }

    std::unique_ptr<ASTNode> parse_primary_expr() {
        if (match(TokenKind::NUMBER)) {
            int val = std::stoi(tokens_[pos_ - 1].lexeme);
            return std::make_unique<NumberExpr>(val);
        }
        if (match(TokenKind::IDENT)) {
            std::string name = tokens_[pos_ - 1].lexeme;
            if (match(TokenKind::LPAREN)) {
                std::vector<std::unique_ptr<ASTNode>> args;
                if (!check(TokenKind::RPAREN)) {
                    args.push_back(parse_expr());
                    while (match(TokenKind::COMMA)) args.push_back(parse_expr());
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
