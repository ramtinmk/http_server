# Sanitizers and Analysis

Sanitizer builds are separate from the default Release build. The CMake options
are independently selectable:

```bash
cmake -S . -B /tmp/http-server-asan \
  -DCMAKE_BUILD_TYPE=Debug -DENABLE_HARDENING=OFF \
  -DENABLE_ASAN=ON -DENABLE_UBSAN=ON
cmake --build /tmp/http-server-asan --parallel 2
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 ./bin/run_tests ring
```

Use `-DENABLE_TSAN=ON` in a separate build. AddressSanitizer and ThreadSanitizer
cannot be enabled together. CI runs ring/TLS suites, the Phase 4 E2E harness,
the server suite, and a short keep-alive load smoke for each sanitizer matrix
entry.

Static analysis and Valgrind run in CI with `make lint` and a server close/error
path. The dependency job uses OSV Scanner recursively over the repository. Local
equivalents require `clang-tidy`, `valgrind`, and `osv-scanner` to be installed.
