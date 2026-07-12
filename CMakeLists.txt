cmake_minimum_required(VERSION 3.16)
project(toyc_compiler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(compiler
    src/main.cpp
    src/defs.h
    src/token.h
    src/lexer.h
    src/ast.h
    src/parser.h
    src/codegen.h
)
