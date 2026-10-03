cmake_minimum_required(VERSION 3.16)

# Project Configuration.
# The version is a real release identifier (see CHANGELOG.md); it is also
# compiled into the binary as BUILD_VERSION and recorded in build-manifest.json.
project(HTTPServer VERSION 1.1.0 LANGUAGES C)

# Default to an optimized build. Without this, CMAKE_BUILD_TYPE is empty and no
# -O flag is applied, which makes the server unusable as a performance baseline
# (and unfair in benchmark comparisons). Pass -DCMAKE_BUILD_TYPE=Debug for an
# unoptimized build with symbols.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
endif()

include(CTest)

# Use C11
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Generate compile_commands.json
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Output Directory Configuration ---
# The build tree is out-of-source in ./build (see GNUmakefile, README, and
# CONTRIBUTING); this keeps the executables in ./bin at the source root so the
# documented ./bin/http_server and ./bin/run_tests commands keep working.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

# --- 1. Find Dependencies ---
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)
# Phase 3: TLS termination uses OpenSSL (libssl + libcrypto).
find_package(OpenSSL REQUIRED)

# --- 2. Compiler Warnings ---
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -Wall 
        -Wextra 
        -pedantic
    )
    # Release uses -O2 rather than CMake's -O3 default: it is the level the
    # benchmark comparisons use and avoids -O3's code-size/register-pressure
    # surprises on this hot loop.
    set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")
endif()

# --- Phase 4: compiler and linker hardening ---
# Build a hard-to-exploit binary: PIE, full RELRO, NX stack, stack-protector,
# stack-clash protection, and _FORTIFY_SOURCE=2. Each flag is probed so an
# older/other toolchain degrades gracefully instead of failing the build, and
# the whole set is controlled by ENABLE_HARDENING for an isolated rollback.
option(ENABLE_HARDENING "Build with compiler/linker hardening flags" ON)
option(ENABLE_FUZZING "Build libFuzzer parser and state-machine targets" OFF)
option(ENABLE_ASAN "Build with AddressSanitizer" OFF)
option(ENABLE_UBSAN "Build with UndefinedBehaviorSanitizer" OFF)
option(ENABLE_TSAN "Build with ThreadSanitizer" OFF)
option(ENABLE_COVERAGE "Build with gcov coverage instrumentation" OFF)

if(ENABLE_ASAN AND ENABLE_TSAN)
    message(FATAL_ERROR "ENABLE_ASAN and ENABLE_TSAN cannot be enabled together")
endif()
if(ENABLE_ASAN OR ENABLE_UBSAN OR ENABLE_TSAN)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "Sanitizers require a GNU or Clang compiler")
    endif()
    set(SANITIZER_NAMES)
    if(ENABLE_ASAN)
        list(APPEND SANITIZER_NAMES address)
    endif()
    if(ENABLE_UBSAN)
        list(APPEND SANITIZER_NAMES undefined)
    endif()
    if(ENABLE_TSAN)
        list(APPEND SANITIZER_NAMES thread)
    endif()
    list(JOIN SANITIZER_NAMES "," SANITIZER_FLAGS)
    add_compile_options(-g -fno-omit-frame-pointer -fsanitize=${SANITIZER_FLAGS})
    add_link_options(-fsanitize=${SANITIZER_FLAGS})
    message(STATUS "Sanitizers enabled: ${SANITIZER_FLAGS}")
endif()

if(ENABLE_COVERAGE)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "Coverage requires a GNU or Clang compiler")
    endif()
    add_compile_options(--coverage -O0 -g)
    add_link_options(--coverage)
    message(STATUS "Coverage instrumentation enabled")
endif()

if(ENABLE_HARDENING AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    include(CheckCCompilerFlag)

    foreach(flag
            -fstack-protector-strong
            -fstack-clash-protection
            -fPIE)
        string(MAKE_C_IDENTIFIER "HAVE${flag}" _have)
        check_c_compiler_flag("${flag}" ${_have})
        if(${_have})
            add_compile_options(${flag})
        endif()
    endforeach()

    # Linker hardening: full RELRO (GOT read-only before main), non-executable
    # stack, and a PIE executable matching -fPIE.
    foreach(flag
            "-pie"
            "-Wl,-z,relro"
            "-Wl,-z,now"
            "-Wl,-z,noexecstack")
        string(MAKE_C_IDENTIFIER "HAVE_LINK${flag}" _have)
        check_c_compiler_flag("${flag}" ${_have})
        if(${_have})
            add_link_options(${flag})
        endif()
    endforeach()

    # _FORTIFY_SOURCE requires optimization; applying it to an unoptimized build
    # produces a warning and no benefit, so scope it to non-Debug configs.
    # -U first so a toolchain default cannot collide with our -D.
    add_compile_options(
        $<$<NOT:$<CONFIG:Debug>>:-U_FORTIFY_SOURCE>
        $<$<NOT:$<CONFIG:Debug>>:-D_FORTIFY_SOURCE=2>
    )

    message(STATUS "Hardening enabled (ENABLE_HARDENING=ON)")
else()
    message(STATUS "Hardening disabled (ENABLE_HARDENING=OFF)")
endif()

# --- 3. Source Management ---
include_directories(include)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

# Explicit source list. This replaces a previous `file(GLOB ...)`: a source file
# that exists on disk but is not listed here is *not* built, and if anything
# references its symbols the link fails loudly. Update this list when adding or
# removing a file, then re-run `cmake -S . -B build`.
#
# SERVER_MAIN is excluded from SOURCES because the fuzz targets link the library
# sources without a second `main()`.
set(SERVER_MAIN "src/main.c")
set(SOURCES
    src/client_limits.c
    src/config.c
    src/event_loop.c
    src/file_cache.c
    src/http_server.c
    src/log.c
    src/memory_profiler.c
    src/metrics.c
    src/path_resolver.c
    src/privilege.c
    src/ring_buffer.c
    src/sandbox.c
    src/sd_notify.c
    src/tls.c
)

# --- Optional Phase 4 fuzzing ---------------------------------------------
# libFuzzer is intentionally outside the default build. Configure this tree
# with CC=clang and -DENABLE_FUZZING=ON; the targets use sanitizer instrumentation
# and are not production server binaries.
if(ENABLE_FUZZING)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "ENABLE_FUZZING requires a Clang compiler with libFuzzer; configure with CC=clang")
    endif()

    set(FUZZ_COMMON ${SOURCES})
    add_executable(http_parser_fuzzer fuzz/http_parser_fuzz.c ${FUZZ_COMMON})
    add_executable(connection_state_fuzzer fuzz/connection_state_fuzz.c ${FUZZ_COMMON})
    foreach(fuzz_target http_parser_fuzzer connection_state_fuzzer)
        target_compile_definitions(${fuzz_target} PRIVATE
            FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION)
        target_compile_options(${fuzz_target} PRIVATE
            -O1 -g -fno-omit-frame-pointer
            -fsanitize=fuzzer-no-link,address,undefined)
        target_link_options(${fuzz_target} PRIVATE
            -fsanitize=fuzzer,address,undefined)
        target_link_libraries(${fuzz_target} PRIVATE
            Threads::Threads ZLIB::ZLIB OpenSSL::SSL OpenSSL::Crypto)
    endforeach()

    add_custom_target(fuzz-smoke
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/fuzz_smoke.py
                --repo-root ${CMAKE_SOURCE_DIR}
        DEPENDS http_parser_fuzzer connection_state_fuzzer
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        USES_TERMINAL
        COMMENT "Running the Phase 4 libFuzzer smoke corpus")
endif()

# --- 4. Target: HTTP Server ---
add_executable(http_server 
    ${SERVER_MAIN}
    ${SOURCES}
)

if(ENABLE_COVERAGE)
    set_target_properties(http_server PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

if(ENABLE_COVERAGE)
    add_custom_target(coverage
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/phase4_coverage.py
                --repo-root ${CMAKE_SOURCE_DIR}
                --build-dir ${CMAKE_BINARY_DIR}
                --baseline ${CMAKE_SOURCE_DIR}/coverage/phase4_baseline.json
                --output ${CMAKE_SOURCE_DIR}/benchmarks/production_phase4_coverage.json
        DEPENDS http_server run_tests
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        USES_TERMINAL
        COMMENT "Measuring and enforcing coverage")
endif()

target_link_libraries(http_server PRIVATE 
    Threads::Threads 
    ZLIB::ZLIB
    OpenSSL::SSL
    OpenSSL::Crypto
)

# Default config file used when neither --config nor HTTP_SERVER_CONFIG is set.
# Baked in as an absolute path so ./bin/http_server picks up the checked-in
# defaults regardless of the process working directory.
target_compile_definitions(http_server PRIVATE
    DEFAULT_CONFIG_FILE="${CMAKE_SOURCE_DIR}/http_server.conf"
    BUILD_VERSION="${PROJECT_VERSION}"
)

# Explicit test source list (see the note on SOURCES above).
set(TEST_SOURCES
    tests/main_test.c
    tests/server_test.c
    tests/test_ring_buffer.c
    tests/tls_test.c
)

# Build the test runner when test sources are present.
if(TEST_SOURCES)
    add_executable(run_tests 
        ${TEST_SOURCES}   # main_test.c, test_ring_buffer.c, etc.
        ${SOURCES}        # ring_buffer.c, etc. (from src/)
    )

    if(ENABLE_COVERAGE)
        set_target_properties(run_tests PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    endif()

    # Allow tests to find header files in tests/ (for test_utils.h)
    target_include_directories(run_tests PRIVATE tests)

    target_link_libraries(run_tests PRIVATE 
        Threads::Threads 
        ZLIB::ZLIB
        OpenSSL::SSL
        OpenSSL::Crypto
    )
    
    message(STATUS "Test runner configured with sources: ${TEST_SOURCES}")

    add_test(NAME unit_tests COMMAND run_tests)
    set_tests_properties(
        unit_tests
        PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
endif()

# --- Build manifest --------------------------------------------------------
# Record the resolved toolchain and dependency versions next to the build so a
# binary can be traced back to exactly what produced it. Written at configure
# time to ${CMAKE_BINARY_DIR}/build-manifest.json; CI uploads it as an artifact.
execute_process(
    COMMAND git -C ${CMAKE_SOURCE_DIR} rev-parse HEAD
    OUTPUT_VARIABLE BUILD_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
execute_process(
    COMMAND git -C ${CMAKE_SOURCE_DIR} rev-parse --abbrev-ref HEAD
    OUTPUT_VARIABLE BUILD_GIT_BRANCH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
execute_process(
    COMMAND git -C ${CMAKE_SOURCE_DIR} status --porcelain
    OUTPUT_VARIABLE BUILD_GIT_STATUS
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(BUILD_GIT_COMMIT STREQUAL "")
    set(BUILD_GIT_COMMIT "unknown")
endif()
if(BUILD_GIT_BRANCH STREQUAL "")
    set(BUILD_GIT_BRANCH "unknown")
endif()
if(BUILD_GIT_STATUS STREQUAL "")
    set(BUILD_GIT_DIRTY false)
else()
    set(BUILD_GIT_DIRTY true)
endif()

# Render a JSON array of quoted, comma-separated entries from a list.
function(_json_array out_var)
    set(_acc "")
    foreach(_item IN LISTS ARGN)
        if(_acc)
            string(APPEND _acc ",\n")
        endif()
        string(APPEND _acc "    \"${_item}\"")
    endforeach()
    set(${out_var} "${_acc}" PARENT_SCOPE)
endfunction()

_json_array(BUILD_MANIFEST_SOURCES ${SOURCES})
_json_array(BUILD_MANIFEST_TEST_SOURCES ${TEST_SOURCES})

configure_file(
    ${CMAKE_SOURCE_DIR}/cmake/build-manifest.json.in
    ${CMAKE_BINARY_DIR}/build-manifest.json
    @ONLY
)

# Convenience target so `make build-manifest` resolves for CI scripts; the file
# itself is produced at configure time.
add_custom_target(build-manifest
    COMMAND ${CMAKE_COMMAND} -E cat ${CMAKE_BINARY_DIR}/build-manifest.json
    COMMENT "Showing ${CMAKE_BINARY_DIR}/build-manifest.json"
    VERBATIM
)

add_test(
    NAME benchmark_python_tests
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_http_benchmark.py
)
set_tests_properties(
    benchmark_python_tests
    PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

add_test(
    NAME benchmark_corpus_tests
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_benchmark_corpus.py
)
set_tests_properties(
    benchmark_corpus_tests
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
        OpenSSL::SSL
        OpenSSL::Crypto
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

# TLS keep-alive throughput benchmark: starts the server with a throwaway
# self-signed certificate on a TLS listener and drives the same load generator
# over TLS, recording hardware_agnostic_rps in benchmarks/tls_benchmark.csv.
# The server certificate is generated by the harness, so no operator material
# is needed.
add_custom_target(benchmark-tls
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/http_benchmark.py
            --start-server --tls --port 8443 --scenario keep-alive-5000
            --warmup 2 --duration 5 --concurrency 32
            --log-file ${CMAKE_SOURCE_DIR}/benchmarks/tls_benchmark.csv
    DEPENDS http_server
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL
    COMMENT "Running TLS keep-alive benchmark (records hardware_agnostic_rps)"
)

# Deterministic file-class corpus: materializes byte-identical assets plus a
# manifest so comparisons run against the same bytes. The tree is generated
# under benchmarks/corpus (git-ignored); see docs/benchmarks.md for the taxonomy.
add_custom_target(corpus
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/benchmark_corpus.py
            --output-dir ${CMAKE_SOURCE_DIR}/benchmarks/corpus --with-gzip
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL
    COMMENT "Generating the deterministic benchmark corpus"
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

# Phase 0 lifecycle acceptance: invalid configuration must fail naming the key,
# runtime keep-alive tuning must take effect, and SIGTERM under load must drain
# without truncation. Writes benchmarks/production_phase0_lifecycle.json.
add_custom_target(phase0-lifecycle
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/phase0_lifecycle_test.py
    DEPENDS http_server
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL
    COMMENT "Running Phase 0 lifecycle acceptance (config + drain)"
)

# Phase 0 above-capacity acceptance: 2x capacity wrk run with zero client
# timeouts and bounded 503s. Writes benchmarks/production_phase0_2x.json.
add_custom_target(phase0-2x
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/phase0_capacity_2x.py
    DEPENDS http_server
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL
    COMMENT "Running Phase 0 2x-capacity overload acceptance"
)

# Phase 0 access-log overhead acceptance: measured <5% on the keep-alive
# scenario. Writes benchmarks/production_phase0_accesslog.json.
add_custom_target(phase0-accesslog
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/phase0_accesslog_test.py
    DEPENDS http_server
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL
    COMMENT "Running Phase 0 access-log overhead acceptance"
)

# Runtime memory-profiler soak: drive a stationary keep-alive load, sample the
# server metrics snapshot, and write benchmarks/memory_soak.json with the RSS
# and heap drift over the final 80% of the run. Exits non-zero when drift
# exceeds the tolerance. See plans/memory-profiler.md.
add_custom_target(memory-soak
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/memory_soak.py
            --start-server --duration 300 --rate 1000 --concurrency 16
            --output ${CMAKE_SOURCE_DIR}/benchmarks/memory_soak.json
    DEPENDS http_server
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL
    COMMENT "Running memory soak (RSS/heap drift artifact)"
)

# Phase 2 static-serving acceptance: traversal/symlink/encoding corpus, large
# file streaming, cache/fd budgets, and doc-root throughput vs the fixed-path
# baseline. Writes benchmarks/production_phase2_static.json.
add_custom_target(phase2-static
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/phase2_static_test.py
    DEPENDS http_server
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL
    COMMENT "Running Phase 2 secure static-serving acceptance"
)

# Phase 3 TLS acceptance: generate a throwaway self-signed cert, serve the
# document root over TLS with ALPN http/1.1, drive a TLS request/body corpus,
# verify a SIGHUP certificate reload drops no connections, and record the
# TLS/plaintext throughput ratio. Writes benchmarks/production_phase3_tls.json.
add_custom_target(phase3-tls
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/phase3_tls_test.py
    DEPENDS http_server
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL
    COMMENT "Running Phase 3 TLS termination acceptance"
)

# Phase 4 hardening acceptance: the binary is PIE/full-RELRO/NX/stack-protected/
# fortified, run_user/run_group are validated, and a configured identity is
# dropped after binding. Writes benchmarks/production_phase4_hardening.json.
add_custom_target(phase4-hardening
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/phase4_hardening_test.py
    DEPENDS http_server
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL
    COMMENT "Running Phase 4 compiler-hardening and privilege-drop acceptance"
)

# Phase 4 sandbox/resource acceptance: exercises Landlock/seccomp startup and
# serving, per-IP admission, slowloris progress deadlines, rlimits, and the
# hardened systemd unit. Writes production_phase4_sandbox_resources.json.
add_custom_target(phase4-sandbox-resources
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/phase4_sandbox_resources_test.py
    DEPENDS http_server
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL
    COMMENT "Running Phase 4 sandbox and resource-control acceptance"
)

# Phase 5 observability acceptance: scrape the Prometheus endpoint, correlate
# request IDs between the response header and the access log, and verify a
# SIGHUP reload keeps a keep-alive connection usable.
# Writes benchmarks/production_phase5_observability.json.
add_custom_target(phase5-observability
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/phase5_observability_test.py
    DEPENDS http_server
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL
    COMMENT "Running Phase 5 observability and operations acceptance"
)

# Phase 6 capacity smoke gate: a fixed low-rate keep-alive run whose
# hardware_agnostic_rps must stay above the checked-in floor in
# benchmarks/ci_baseline.json. Used by CI and runnable locally.
# Writes benchmarks/ci_capacity_gate.json.
add_custom_target(capacity-smoke
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/ci_capacity_gate.py
    DEPENDS http_server
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL
    COMMENT "Running the Phase 6 CI capacity smoke gate"
)

# Phase 6 capacity report: fresh plaintext fixed-rate, TLS fixed-rate, and
# above-capacity overload runs, re-verifying the scaling-plan 5000 req/s
# acceptance and the bounded overload contract. Writes
# benchmarks/production_phase6_capacity.json.
add_custom_target(phase6-capacity
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/phase6_capacity_report.py
    DEPENDS http_server
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL
    COMMENT "Running the Phase 6 capacity report (plaintext + TLS + overload)"
)

# Phase 6 multi-hour soak: stationary keep-alive load that must leave RSS and
# open file descriptors flat (within 5% over the final 80% of the run). The
# default is three hours; pass `--duration` to memory_soak.py directly for a
# shorter control. Writes benchmarks/production_phase6_soak.json.
add_custom_target(soak
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/memory_soak.py
            --start-server --duration 10800 --rate 1000 --concurrency 16
            --interval 60
            --output ${CMAKE_SOURCE_DIR}/benchmarks/production_phase6_soak.json
    DEPENDS http_server
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL
    COMMENT "Running the Phase 6 multi-hour soak (3h; RSS/FD drift)"
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

    # Static analysis does not need the hardening flags, and an older clang-tidy
    # (e.g. 10.0) rejects a flag its bundled clang does not know
    # (-fstack-clash-protection), turning every file into a compiler error. Run
    # clang-tidy against a separate compile database configured with hardening
    # off so the analyzer is independent of the optimization/hardening surface.
    set(LINT_DB_DIR ${CMAKE_BINARY_DIR}/.lint-db)

    # Report only — no changes to source files
    add_custom_target(lint
        COMMAND ${CMAKE_COMMAND}
            -S ${CMAKE_SOURCE_DIR} -B ${LINT_DB_DIR}
            -DENABLE_HARDENING=OFF
        COMMAND ${CLANG_TIDY_EXE}
            -p ${LINT_DB_DIR}
            ${LINT_SOURCES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Running clang-tidy (report only)..."
        VERBATIM
    )

    # Apply all auto-fixable suggestions in-place
    add_custom_target(lint-fix
        COMMAND ${CMAKE_COMMAND}
            -S ${CMAKE_SOURCE_DIR} -B ${LINT_DB_DIR}
            -DENABLE_HARDENING=OFF
        COMMAND ${CLANG_TIDY_EXE}
            -p ${LINT_DB_DIR}
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

# --- Installation ----------------------------------------------------------
# `cmake --install build` (or `make install`) installs the hardened binary, the
# shipped alias assets, the docs, and the systemd/logrotate/tmpfiles/sysusers
# integration. The default prefix is /usr/local and the unit and installed
# config assume that prefix; DESTDIR is honored for staged/packaged installs.
include(GNUInstallDirs)

install(TARGETS http_server RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Alias assets for /home and /hello. The unit sets
# WorkingDirectory=/usr/local/share/http-server so these resolve.
install(DIRECTORY ${CMAKE_SOURCE_DIR}/root/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/http-server/root
        FILES_MATCHING PATTERN "*.html")

install(FILES
        ${CMAKE_SOURCE_DIR}/readme.md
        ${CMAKE_SOURCE_DIR}/CHANGELOG.md
        ${CMAKE_SOURCE_DIR}/LICENSE
        DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/http-server)

# System integration. Absolute DESTINATIONs are deliberate: /etc and /usr/lib
# are conventional locations independent of the install prefix, and DESTDIR
# still stages them for packaging.
install(FILES ${CMAKE_SOURCE_DIR}/deploy/http-server.service
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/system)
install(FILES ${CMAKE_SOURCE_DIR}/deploy/http_server.conf
        DESTINATION /etc/http-server)
install(FILES ${CMAKE_SOURCE_DIR}/deploy/logrotate.conf
        DESTINATION /etc/logrotate.d RENAME http-server)
install(FILES ${CMAKE_SOURCE_DIR}/deploy/http-server.sysusers
        DESTINATION /usr/lib/sysusers.d RENAME http-server.conf)
install(FILES ${CMAKE_SOURCE_DIR}/deploy/http-server.tmpfiles
        DESTINATION /usr/lib/tmpfiles.d RENAME http-server.conf)

# --- Uninstallation --------------------------------------------------------
configure_file(
    ${CMAKE_SOURCE_DIR}/cmake/cmake_uninstall.cmake.in
    ${CMAKE_BINARY_DIR}/cmake_uninstall.cmake
    @ONLY
)
add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_BINARY_DIR}/cmake_uninstall.cmake
    COMMENT "Uninstalling files recorded in install_manifest.txt"
)
