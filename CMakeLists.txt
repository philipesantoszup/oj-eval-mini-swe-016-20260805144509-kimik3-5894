cmake_minimum_required(VERSION 3.10)
project(bpt_file_storage CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-O2)

add_executable(code main.cpp)
