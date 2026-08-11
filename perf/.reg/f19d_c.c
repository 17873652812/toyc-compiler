int add2(int a, int b) { return a + b; }
int main_test() {
    int r = add2(add2(1,2), add2(3,4)) + add2(5, add2(6,7));
    // add2(3,7)=10, add2(5,13)=18, 10+18=28
    return r;
}
int main() { return (unsigned char)main_test(); }
