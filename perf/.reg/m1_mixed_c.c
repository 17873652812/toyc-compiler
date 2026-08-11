int counter = 0;
int gcd(int a, int b) {
    while (b != 0) {
        int t = b;
        b = a % b;
        a = t;
    }
    return a;
}
int fib(int n) {
    if (n <= 1) return n;
    if (n == 2) return 1;
    return fib(n - 1) + fib(n - 2);
}
int main_test() {
    counter = counter + 1;
    int g = gcd(48, 18);
    int f = fib(12);
    int acc = 0;
    int i = 0;
    while (i < 100) {
        if (i % 3 == 0 && i % 5 == 0) acc = acc + 1;
        i = i + 1;
    }
    return g * 10000 + f * 100 + acc + counter;
}
int main() { return (unsigned char)main_test(); }
