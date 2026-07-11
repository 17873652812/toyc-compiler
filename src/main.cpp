
  #include <iostream>
  #include <string>
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

