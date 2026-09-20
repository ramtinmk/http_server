# Educational HTTP Server in C

A simple HTTP server implementation for educational purposes demonstrating:
- Basic socket programming
- HTTP protocol handling
- Concurrent connection management using forking
- Signal handling

## Features

- Handles multiple concurrent connections using process forking
- Responds to GET requests with basic HTML page
- Displays requested path and method
- Proper signal handling for zombie processes
- Port reuse capability (SO_REUSEADDR)

## Prerequisites

- C compiler (gcc recommended)
- POSIX-compliant operating system (Linux/macOS)
- Basic understanding of networking concepts

## Building

```
make
./bin/http_server
```

## Tests and Benchmarking

The unit suites do not require a running server:

```
./bin/run_tests ring
./bin/run_tests thread_pool
ctest --output-on-failure
```

The server suite requires `./bin/http_server` to be running from the project
root. Request-rate benchmark targets start and stop their own server:

```
make benchmark         # 1,000 req/s new-connection smoke test
make stress            # 5,000 req/s new-connection, logs stress_results.csv
make benchmark-matrix  # every scenario from plans/scaling-plan.md
```

The benchmark implements the measurement contract in `plans/scaling-plan.md`.
Each run records the offered rate, completed/successful/failed requests,
status-code distribution, latency percentiles, connection and keep-alive
request rates, server CPU/RSS/open file descriptors, thread-pool queue depth,
active workers, rejected tasks, context switches, and TCP retransmits. Results
are appended to `benchmarks/*.csv`; JSON Lines is written when `--log-file`
ends in `.json`/`.jsonl`. Server-side counters are exposed through the
`HTTP_SERVER_METRICS_FILE` environment variable and access logging is disabled
while the benchmark owns the server (`HTTP_SERVER_ACCESS_LOG=0`).

The selectable scenarios are `new-connection-1000`, `new-connection-5000`,
`keep-alive-5000`, `mixed-paths-5000`, `gzip-500`, `error-paths`, and
`slow-clients`. Custom runs can combine `--rate`, `--duration`, `--path`
(repeatable for a path mix), `--keep-alive`, `--gzip`, and `--expect-status`:

```
python3 scripts/http_benchmark.py --start-server --scenario mixed-paths-5000 \
  --duration 30 --concurrency 32
```

The benchmark uses only Python's standard library and validates response
framing (Content-Length, chunked, or `Connection: close`) and expected status
codes. A nonzero exit status means that a request failed, returned an
unexpected status, or missed the minimum throughput threshold.

### `wrk` Snapshot (2026-09-20)

One local run against a running Phase 3 epoll server served `/home` for 30 seconds:

```
wrk -t12 -c400 -d30s --latency http://127.0.0.1:8081/home
```

- Requests: 3,941,992
- Throughput: 131,017.52 requests/sec
- Latency: 3.33 ms p50, 5.04 ms p99, 17.39 ms max
- Transfer: 1.24 GB total, 42.10 MB/sec

This is an indicative local snapshot, not a replacement for the canonical
measurement-contract results recorded by `scripts/http_benchmark.py`.

### **Phase 1: Protocol Compliance**
1. **Proper HTTP Header Parsing**   ✅
   - Parse full request headers into key-value pairs
   - Handle `Host`, `User-Agent`, and `Content-Length` headers
   - Example structure:
   ```c
   typedef struct {
       char method[8];
       char path[1024];
       char headers[32][2][256]; // [header_count][key/value]
       int header_count;
   } HTTPRequest;
   ```

2. **HTTP Error Handling**  ✅
   - Implement 400 (Bad Request), 404 (Not Found), 501 (Not Implemented)
   - Create error template HTML responses
   - Handle malformed requests gracefully

3. **HTTP/1.1 Keep-Alive Support**  ✅
   - Add `Connection: keep-alive` header
   - Implement request pipelining
   - Use content-length for proper message delimitation

---

### **Phase 2: Concurrency Improvements**
4. **Thread Pool Implementation**  ✅
   - Replace `fork()` with POSIX threads (`pthread`)
   - Create worker thread pool with task queue
   - Use mutexes and condition variables for synchronization

5. **Event-Driven Architecture**  
   - Implement using `epoll` (Linux) or `kqueue` (BSD)
   - Non-blocking I/O with state machines
   - Compare performance against process/thread models

---

### **Phase 3: Performance Optimization**
6. **Zero-Copy File Transfer**  ✅
   Implement using `sendfile()` system call:
   ```c
   int file_fd = open(filename, O_RDONLY);
   off_t offset = 0;
   sendfile(client_socket, file_fd, &offset, file_size);
   ```

7. **Buffer Management**  
   - Create ring buffer for request/response handling ✅
   - Implement dynamic buffer resizing ✅
   - Add read/write timeout protection

8. **HTTP Compression**  ✅
   - Add gzip compression using zlib
   - Handle `Accept-Encoding` header
   - Compress responses on-the-fly

---

### **Phase 4: Security Enhancements**
9. **TLS Support**  
   - Integrate OpenSSL library
   - Implement HTTPS on port 443
   - Handle SSL handshake and encryption

10. **Request Validation**  
    - Add maximum request size limit
    - Sanitize file paths to prevent directory traversal
    - Implement basic rate limiting

---

### **Phase 5: Advanced Features**
11. **Static File Serving**  
    - Serve files from document root directory
    - Handle MIME types (Content-Type header)
    - Add directory listing support

12. **CGI Support**  
    - Execute external programs via fork/exec
    - Handle environment variables:
    ```c
    setenv("REQUEST_METHOD", method, 1);
    setenv("QUERY_STRING", query, 1);
    ```

13. **Reverse Proxy**  
    - Forward requests to backend servers
    - Handle load balancing between multiple backends
    - Add caching layer

---

### **Phase 6: Production Readiness**
14. **Configuration System**  
    - Read config file (JSON/INI format)
    - Support virtual hosts
    - Hot reload without restart

15. **Logging & Monitoring**  
    - Implement access logs (Common Log Format)
    - Add metrics collection (requests/sec, latency)
    - Create health check endpoint

16. **Benchmarking**  
    - Compare with nginx using `wrk`:
    ```bash
    wrk -t12 -c400 -d30s http://localhost:8081/
    ```
    - Profile with `perf`/`valgrind`

---

### **Learning Milestones**
1. **Networking Foundations**  
   - Complete Phases 1-3 → Understand HTTP/TCP stack

2. **Systems Programming**  
   - Complete Phase 2 & 5 → Master concurrency/I/O

3. **Production Engineering**  
   - Complete Phase 6 → Learn deployment concerns
