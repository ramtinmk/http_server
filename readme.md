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
./http_server
```
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
4. **Thread Pool Implementation**  
   - Replace `fork()` with POSIX threads (`pthread`)
   - Create worker thread pool with task queue
   - Use mutexes and condition variables for synchronization

5. **Event-Driven Architecture**  
   - Implement using `epoll` (Linux) or `kqueue` (BSD)
   - Non-blocking I/O with state machines
   - Compare performance against process/thread models

---

### **Phase 3: Performance Optimization**
6. **Zero-Copy File Transfer**  
   Implement using `sendfile()` system call:
   ```c
   int file_fd = open(filename, O_RDONLY);
   off_t offset = 0;
   sendfile(client_socket, file_fd, &offset, file_size);
   ```

7. **Buffer Management**  
   - Create ring buffer for request/response handling
   - Implement dynamic buffer resizing
   - Add read/write timeout protection

8. **HTTP Compression**  
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
    wrk -t12 -c400 -d30s http://localhost:8080/
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

