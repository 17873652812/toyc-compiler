int total = 0;
int main_test() {
    int i = 0;
    while (i < 50) {
        if (i % 2 == 0 && i > 10) total = total + i;
        i = i + 1;
    }
    return total % 256;
}
int main() { return (unsigned char)main_test(); }
