int add2(int a, int b) { return a + b; }
int mul2(int a, int b) { return a * b; }
int main_test() {
    int r = add2(mul2(add2(1,2), add2(3,4)), mul2(5,6));
    // add2(1,2)=3, add2(3,4)=7, mul2(3,7)=21, mul2(5,6)=30, add2(21,30)=51
    return r;
}
int main() { return (unsigned char)main_test(); }
