int main_test() {
    int i = 0;
    while (1) {
        i = i + 1;
        if (i >= 5) break;
    }
    return i;   // 5
}
int main() { return (unsigned char)main_test(); }
