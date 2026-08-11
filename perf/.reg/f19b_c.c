int rec(int n, int acc) {
    if (n <= 0) return acc;
    return rec(n - 1, acc + n);
}
int main_test() {
    return rec(100, 0);
}
int main() { return (unsigned char)main_test(); }
