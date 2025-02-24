# CMake minimum version required
cmake_minimum_required(VERSION 3.10)

# Project name and version
project(HTTPServer VERSION 1.0)

# Set C standard (C99 or later)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Add the executable
add_executable(http_server http_server.c)

add_executable(test test.c)

# Enable warnings
if(CMAKE_COMPILER_IS_GNUCC OR CMAKE_C_COMPILER_ID MATCHES "Clang")
    target_compile_options(http_server PRIVATE -Wall -Wextra -pedantic)
endif()