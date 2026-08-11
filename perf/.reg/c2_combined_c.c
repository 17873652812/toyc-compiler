int main_test() {
    int a = 3;
    int b = 4;
    if (a < b) {
        return a * 100 + b;    // 304
    } else {
        return 0;
    }
    return 999;
}
int main() { return (unsigned char)main_test(); }
