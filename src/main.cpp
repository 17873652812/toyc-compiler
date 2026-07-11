
  #include <iostream>
  #include <string>
   #include "defs.h"
  using namespace toyc;
  using namespace std;

  int main() {
      string source;
      string line;
      while (getline(cin, line)) {
          source += line + '\n';
      }

      cout << "Read " << source.size() << " chars." << endl;
      return 0;
  }

