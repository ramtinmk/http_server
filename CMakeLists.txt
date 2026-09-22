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

find_package(Python3 COMPONENTS Interpreter REQUIRED)

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

add_test(
    NAME benchmark_python_tests
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_http_benchmark.py
)
set_tests_properties(
    benchmark_python_tests
    PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

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
add_custom_target(benchmark
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/http_benchmark.py
            --start-server --scenario new-connection-1000
            --duration 5 --concurrency 16
    DEPENDS http_server
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL
    COMMENT "Running HTTP benchmark (1000 req/s for 5 seconds)"
)

# Canonical 5,000 req/s new-connection stress run described by
# plans/scaling-plan.md (50,000 offered requests). The acceptance gate uses
# hardware_agnostic_rps (successful requests per server CPU-second), not raw
# wall-clock throughput, so it remains meaningful across host hardware.
add_custom_target(stress
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/http_benchmark.py
            --start-server --scenario new-connection-5000
            --duration 10 --concurrency 32
            --timeout 30 --calibrate on
            --min-hardware-agnostic-rps 1000
            --log-file ${CMAKE_SOURCE_DIR}/benchmarks/stress_results.csv
    DEPENDS http_server
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL
    COMMENT "Running HTTP request-rate stress test (5000 req/s for 10 seconds)"
)

# Runs every scenario from the measurement contract and appends each result to
# a single CSV for phase-over-phase comparison.
add_custom_target(benchmark-matrix
    COMMAND bash ${CMAKE_SOURCE_DIR}/scripts/run_benchmark_matrix.sh
            ${CMAKE_SOURCE_DIR}/benchmarks/benchmark_matrix.csv 5
    DEPENDS http_server
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL
    COMMENT "Running the scaling-plan benchmark matrix"
)

# Phase 4 saturation acceptance: drive -c below, equal to, and above the
# configured connection capacity and verify the bounded overload outcome and
# post-drain cleanup. Writes benchmarks/phase4_saturation.json.
add_custom_target(saturation
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/saturation_test.py
            --capacity 16
    DEPENDS http_server
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL
    COMMENT "Running Phase 4 saturation acceptance (capacity 16)"
)

# Phase 4: static assets are precompressed/cached at startup and startup must
# fail fast when one cannot be loaded.
add_custom_target(startup-failfast
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/startup_failfast_test.py
    DEPENDS http_server
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL
    COMMENT "Checking startup fail-fast on missing static assets"
)

# --- Linting with clang-tidy ---
find_program(CLANG_TIDY_EXE NAMES clang-tidy clang-tidy-18 clang-tidy-17 clang-tidy-16)

if(CLANG_TIDY_EXE)
    set(LINT_SOURCES ${SOURCES} ${SERVER_MAIN})

    # Report only — no changes to source files
    add_custom_target(lint
        COMMAND ${CLANG_TIDY_EXE}
            -p ${CMAKE_SOURCE_DIR}
            ${LINT_SOURCES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Running clang-tidy (report only)..."
        VERBATIM
    )

    # Apply all auto-fixable suggestions in-place
    add_custom_target(lint-fix
        COMMAND ${CLANG_TIDY_EXE}
            -p ${CMAKE_SOURCE_DIR}
            --fix
            --fix-errors
            ${LINT_SOURCES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Running clang-tidy with auto-fix..."
        VERBATIM
    )
    message(STATUS "clang-tidy found: ${CLANG_TIDY_EXE} — run 'make lint' or 'make lint-fix'")
else()
    message(WARNING "clang-tidy not found; install with: apt install clang-tidy")
endif()
