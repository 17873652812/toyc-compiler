int main_test() {
    int big = 2000000000;
    int x = big + big;       // 溢出成 -294967296
    int y = big * 2;         // 溢出
    return (x + y) % 256;
}
int main() { return (unsigned char)main_test(); }
