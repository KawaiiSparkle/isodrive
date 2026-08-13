cmake_minimum_required(VERSION 3.15)
project(isodrive LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_options(-Wall -Wextra -O2)
include_directories(src/include)
add_executable(isodrive
    src/main.cpp
    src/util.cpp
    src/logger.cpp
    src/configfsisomanager.cpp
    src/androidusbisomanager.cpp
)
