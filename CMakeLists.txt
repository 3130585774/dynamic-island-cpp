cmake_minimum_required(VERSION 3.20)

project(cpp_dynamic_island VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(cpp_dynamic_island WIN32
    src/main.cpp
)

target_compile_definitions(cpp_dynamic_island PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

target_link_libraries(cpp_dynamic_island PRIVATE
    d2d1
    dwrite
    dwmapi
    windowscodecs
)
