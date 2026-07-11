 #pragma once

  #include <string>
  #include <iostream>
  #include <cstdint>
  #include <vector>
  #include <memory>

  namespace toyc {

  // i32 = 32 位整数（ToyC 的 int 就是这样）
  using i32 = int32_t;

  // 位置信息：记录每个 Token 在源码的第几行第几列
  struct Position {
      int line{1};   // 行号，默认 1
      int col{1};    // 列号，默认 1
  };

  }  // namespace toyc