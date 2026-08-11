int main_test() {
    int x = 1 + 2 * 3;
    int y = x * 4;
    if (1) return y + 5;
    return 999;
}
int main() { return (unsigned char)main_test(); }
