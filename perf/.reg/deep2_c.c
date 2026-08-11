int helper(int x) { return x * 2; }
int f(int n) {
    if (n <= 0) return 1;
    return helper(f(n - 1)) + helper(n);
}
int main_test() {
    return f(5);
}
int main() { return (unsigned char)main_test(); }
