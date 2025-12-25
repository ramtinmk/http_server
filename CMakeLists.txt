cmake_minimum_required(VERSION 3.16)

# Project Configuration
project(HTTPServer VERSION 1.0 LANGUAGES C)

# Use C11
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Generate compile_commands.json
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- NEW: Output Directory Configuration ---
# This tells CMake to put all executables in ./bin/ instead of ./build/
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

# --- 1. Find Dependencies ---
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)

# --- 2. Compiler Warnings ---
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -Wall 
        -Wextra 
        -pedantic
    )
endif()

# --- 3. Source Management ---
include_directories(include)

# Automatically find all .c files in src/
file(GLOB SOURCES "src/*.c")

# Separate main.c from the rest of the logic logic
set(SERVER_MAIN "src/main.c")
list(REMOVE_ITEM SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/${SERVER_MAIN}")

# --- 4. Target: HTTP Server ---
add_executable(http_server 
    ${SERVER_MAIN}
    ${SOURCES}
)

target_link_libraries(http_server PRIVATE 
    Threads::Threads 
    ZLIB::ZLIB
)

# --- 5. Target: Tests ---
# Note: Ensure test.c is either in root or tests/ folder. 
# Adjusted here to look in both for safety based on previous steps.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test.c")
    set(TEST_FILE "tests/test.c")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test.c")
    set(TEST_FILE "test.c")
endif()

if(DEFINED TEST_FILE)
    add_executable(run_tests 
        ${TEST_FILE}
        ${SOURCES}
    )

    target_link_libraries(run_tests PRIVATE 
        Threads::Threads 
        ZLIB::ZLIB
    )
endif()