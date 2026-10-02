# Coverage

Coverage is an isolated GCC/Clang `gcov` build option. It does not modify the
default Release build:

```bash
cmake -S . -B /tmp/http-server-coverage \
  -DCMAKE_BUILD_TYPE=Debug -DENABLE_HARDENING=OFF \
  -DENABLE_COVERAGE=ON
cmake --build /tmp/http-server-coverage --target coverage --parallel 2
```

The `coverage` target runs the ring and TLS focused suites plus the server E2E
suite, merges execution data from the test runner and server binaries, and
reports line/function coverage for:

- `src/http_server.c` (parser and response preparation)
- `src/path_resolver.c`
- `src/event_loop.c` (connection state machine)

The checked-in floor is `coverage/phase4_baseline.json`. The JSON evidence is
written to `benchmarks/production_phase4_coverage.json`; the target fails when
any measured line or function percentage drops below its floor. CI runs this
target and uploads the artifact.
