cmake_minimum_required(VERSION 3.16)

# Project Configuration
project(HTTPServer VERSION 1.0 LANGUAGES C)

include(CTest)

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

file(GLOB TEST_SOURCES "tests/*.c")

# If we found test files, build the test runner
if(TEST_SOURCES)
    add_executable(run_tests 
        ${TEST_SOURCES}   # main_test.c, test_ring_buffer.c, etc.
        ${SOURCES}        # ring_buffer.c, etc. (from src/)
    )

    # Allow tests to find header files in tests/ (for test_utils.h)
    target_include_directories(run_tests PRIVATE tests)

    target_link_libraries(run_tests PRIVATE 
        Threads::Threads 
        ZLIB::ZLIB
    )
    
    message(STATUS "Test runner configured with sources: ${TEST_SOURCES}")

    # add_test(NAME ring_buffer_tests COMMAND run_tests ring)
    # add_test(NAME thread_pool_tests COMMAND run_tests thread_pool)
    add_test(NAME unit_tests COMMAND run_tests)
    set_tests_properties(
        # ring_buffer_tests
        # thread_pool_tests
        unit_tests
        PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
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

# Dependency-free request-rate benchmark. These targets start the server in
# the repository root so relative static-file paths behave like production.
find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_custom_target(benchmark
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/http_benchmark.py
            --start-server --rate 100 --duration 5 --concurrency 16
    DEPENDS http_server
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL
    COMMENT "Running HTTP benchmark (100 req/s for 5 seconds)"
)

add_custom_target(stress
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/http_benchmark.py
            --start-server --rate 5000 --duration 10 --concurrency 32
            --timeout 30 --min-rps 400
            --log-file ${CMAKE_SOURCE_DIR}/benchmarks/stress_results.csv
    DEPENDS http_server
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL
    COMMENT "Running HTTP request-rate stress test (5000 req/s for 10 seconds)"
)
