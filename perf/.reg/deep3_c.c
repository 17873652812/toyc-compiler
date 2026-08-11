int inc(int x) { return x + 1; }
int main_test() {
    int r = inc(inc(inc(inc(inc(inc(10))))));
    return r;   // 16
}
int main() { return (unsigned char)main_test(); }
