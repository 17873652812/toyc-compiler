 #pragma once

  #include "token.h"
  #include <cctype>

  namespace toyc {

  class Lexer {
  public:
      explicit Lexer(std::string src)
          : source_(std::move(src)), pos_(0), line_(1), col_(1) {}

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
      std::string source_;
      size_t pos_;
      int line_;
      int col_;

      char peek() const {
          if (pos_ >= source_.size()) return '\0';
          return source_[pos_];
      }

      char advance() {
          if (pos_ >= source_.size()) return '\0';
          char c = source_[pos_++];
          if (c == '\n') { line_++; col_ = 1; }
          else { col_++; }
          return c;
      }

      Position current_pos() const {
          return Position{line_, col_};
      }

      void skip_whitespace() {
          while (pos_ < source_.size()) {
              char c = peek();
              if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                  advance();
              else
                  break;
          }
      }

      Token next_token() {
          skip_whitespace();

          if (pos_ >= source_.size())
              return make(TokenKind::END, "");

          Position p = current_pos();
          char c = peek();

          // 字母或下划线 → 关键字或标识符
          if (std::isalpha(c) || c == '_') {
              std::string word;
              while (std::isalnum(peek()) || peek() == '_')
                  word += advance();

              auto it = keywords.find(word);
              if (it != keywords.end())
                  return make(it->second, word, p);
              else
                  return make(TokenKind::IDENT, word, p);
          }

          // 数字
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
              default:  return make(TokenKind::ERR, std::string(1, c),
  p);
          }
      }

      Token make(TokenKind k, std::string lex, Position p) {
          return Token(k, std::move(lex), p);
      }
      Token make(TokenKind k, std::string lex) {
          return Token(k, std::move(lex), current_pos());
      }
  };

  }  // namespace toyc
