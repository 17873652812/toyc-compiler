// ============================================================
// defs.h —— 公共定义
// 整个编译器各模块共享的基础类型定义。
// ============================================================
#pragma once

#include <string>
#include <iostream>
#include <cstdint>
#include <vector>
#include <memory>

namespace toyc {

// i32：32 位有符号整数（ToyC 的 int 就是这个）
using i32 = int32_t;

// Position：源码中的位置（行号 + 列号），报错时用
struct Position {
    int line{1};
    int col{1};
};

}  // namespace toyc
