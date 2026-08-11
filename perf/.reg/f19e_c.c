int add2(int a, int b) { return a + b; }
int main_test() {
    int i = 0;
    int acc = 0;
    while (i < 1000) {
        acc = add2(acc, i);
        i = i + 1;
    }
    return acc % 256;
}
int main() { return (unsigned char)main_test(); }
