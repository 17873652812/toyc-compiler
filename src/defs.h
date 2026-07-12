#pragma once
// ↑ 防止这个文件被重复引入（每个 .h 文件第一行都写它）

// ========== 引入 C++ 自带工具 ==========
#include <string>       // std::string — 字符串（文字）
#include <iostream>     // std::cin / std::cout — 输入输出
#include <cstdint>      // int32_t — 32位整数
#include <vector>       // std::vector — 动态数组（可以变长的列表）
#include <memory>       // std::unique_ptr — 独占指针（后面 AST 里用）

// ========== 我们编译器的命名空间 ==========
namespace toyc {
// namespace = 给代码起个姓，防止跟别人的代码名字冲突
// 用的时候写 toyc::Position，就知道是我们定义的类型

// ---- i32：32 位有符号整数 ----
// ToyC 语言里的 int 就是 32 位的，所以直接给 int32_t 起个小名叫 i32
using i32 = int32_t;

// ---- Position：源码中的位置 ----
// 记录第几行第几列，报错的时候告诉用户"你在第 5 行第 10 列出错了"
struct Position {
    int line{1};  // 行号，从 1 开始，默认第 1 行
    int col{1};   // 列号，从 1 开始，默认第 1 列
};

}  // namespace toyc — 结束
