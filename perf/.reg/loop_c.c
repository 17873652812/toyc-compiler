int main_test() {
    int sum = 0;
    int i = 0;
    while (i < 100000) {
        sum = sum + i;
        i = i + 1;
    }
    return sum % 100;
}
int main() { return (unsigned char)main_test(); }
