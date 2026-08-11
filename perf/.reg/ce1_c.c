int main_test() {
    int a = 1;
    int b = 0;
    if (1) b = a * 10;
    else b = a * 100;
    if (0) b = b + 1;
    else b = b + 2;
    return b;   // 10 + 2 = 12
}
int main() { return (unsigned char)main_test(); }
