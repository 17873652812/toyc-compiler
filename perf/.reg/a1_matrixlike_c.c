int total = 0;
int compute(int a, int b) {
    int acc = 0;
    int i = 0;
    while (i < 10) {
        int j = 0;
        while (j < 10) {
            acc = acc + a * b + i - j;
            j = j + 1;
        }
        i = i + 1;
    }
    total = total + acc;
    return acc;
}
int main_test() {
    int r = compute(3, 4) + compute(5, 6);
    return r + total;
}
int main() { return (unsigned char)main_test(); }
