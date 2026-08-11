int main_test() {
    int x = 5;
    int y = 0;
    if (1) { int z = 100; y = x + z; }
    else { int w = 200; y = x + w; }
    return y;   // 105
}
int main() { return (unsigned char)main_test(); }
