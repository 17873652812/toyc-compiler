int main_test() {
    int a = -7;
    int b = 3;
    int q = a / b;      // -7/3 = -2 (向零截断)
    int r = a % b;      // -7%3 = -1
    int c = -13;
    int q2 = c / -4;    // -13/-4 = 3
    int r2 = c % -4;    // -13%-4 = -1
    return (q * 1000 + r * 100 + q2 * 10 + r2 + 1000) % 256;
}
int main() { return (unsigned char)main_test(); }
