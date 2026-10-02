# Fuzzing

Phase 4e fuzzing is opt-in and requires a Clang toolchain with libFuzzer.
The default Release build does not compile or link sanitizer instrumentation.

## Build

```bash
CC=clang CXX=clang++ cmake -S . -B /tmp/http-server-fuzz \
  -DCMAKE_BUILD_TYPE=Debug -DENABLE_HARDENING=OFF -DENABLE_FUZZING=ON
cmake --build /tmp/http-server-fuzz --target http_parser_fuzzer connection_state_fuzzer
```

The parser target exercises transactional request parsing and response-resource
cleanup. The connection-state target feeds arbitrary fragments through the
production parser while modeling queue drain, keep-alive, and protocol-error
transitions without opening sockets per input.

## Smoke run

```bash
cmake --build /tmp/http-server-fuzz --target fuzz-smoke
```

The target runs 1,000 inputs per target over the checked-in seed corpus and
writes `benchmarks/production_phase4_fuzz.json`. For longer campaigns, invoke
either fuzzer directly with its corpus directory and retain any generated
crash/reproducer files as separate evidence.
