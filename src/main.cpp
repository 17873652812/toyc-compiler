
  #include <iostream>
  #include <string>
  #include "defs.h"
  #include "token.h"
  using namespace std;
  using namespace toyc;

  int main() {
      string source;
      string line;
      while (getline(cin, line)) {
          source += line + '\n';
      }

      // 测试：手动创建一个 Token，看看能不能打印
      Token t(TokenKind::KW_INT, "int", Position{1, 1});
      cout << "kind: " << t.kind_name() << endl;
      cout << "text: " << t.lexeme << endl;
      cout << "pos:  line " << t.pos.line << ", col " << t.pos.col <<
  endl;

      return 0;
  }