int main_test() {
    int a = 5;
    int b = 1;
    int c = 4;
    int d = 2;
    int e = 8;
    int swapped = 1;
    while (swapped) {
        swapped = 0;
        if (a > b) { int t = a; a = b; b = t; swapped = 1; }
        if (b > c) { int t = b; b = c; c = t; swapped = 1; }
        if (c > d) { int t = c; c = d; d = t; swapped = 1; }
        if (d > e) { int t = d; d = e; e = t; swapped = 1; }
    }
    return a * 10000 + b * 1000 + c * 100 + d * 10 + e;
}
int main() { return (unsigned char)main_test(); }
