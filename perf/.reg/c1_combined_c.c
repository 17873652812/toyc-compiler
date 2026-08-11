int g = 0;
int main_test() {
    int x = 5;
    int y = x * 2 + 10;       // 折叠部分
    int i = 0;
    while (i < y) {           // y=20
        g = g + i;
        i = i + 1;
    }
    return g;                  // 0+1+...+19 = 190 → 190 & 255 = 190
}
int main() { return (unsigned char)main_test(); }
