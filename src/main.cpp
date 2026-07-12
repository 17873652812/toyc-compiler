  #include <iostream>
  #include <string>
  #include "defs.h"
  #include "token.h"
  #include "lexer.h"
  using namespace std;
  using namespace toyc;

  int main() {
      string source;
      string line;
      while (getline(cin, line)) {
          source += line + '\n';
      }

      if (source.empty()) {
          cerr << "empty input" << endl;
          return 1;
      }

      // 用 Lexer 把源码切成 Token
      Lexer lexer(source);
      auto tokens = lexer.tokenize();

      // 把每个 Token 打印出来
      for (const auto& tok : tokens) {
          cout << tok.kind_name()
               << " '" << tok.lexeme << "'"
               << "  at " << tok.pos.line << ":" << tok.pos.col
               << endl;
      }

      return 0;
  }
