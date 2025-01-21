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
./http_server.sh
```