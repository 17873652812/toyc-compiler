int visited = 0;
int dfs(int node, int depth) {
    visited = visited + 1;
    if (depth == 0) return node;
    if (node % 2 == 0) return dfs(node / 2, depth - 1);
    return dfs(node * 3 + 1, depth - 1);
}
int main_test() {
    int a = dfs(10, 5);
    int b = dfs(7, 4);
    return a * 100 + b + visited;
}
int main() { return (unsigned char)main_test(); }
