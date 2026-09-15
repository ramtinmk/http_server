// http_server.h
#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <signal.h>
#include <pthread.h>
#include <fcntl.h>    // For open() flags like O_RDONLY
#include <sys/time.h> // Required for struct timeval in select
#include <errno.h>    // For errno
#include <sys/stat.h>   // Needed for struct stat and fstat
#include <sys/sendfile.h> // Needed for sendfile
#include <zlib.h>   // For gzip compression
#include <string.h> // For strstr, strcasecmp

#include "ring_buffer.h"
#include "thread_pool.h"

// --- Configuration and Constants ---
#define PORT 8080
#define BACKLOG 10
#define BUFFER_SIZE 8092
#define ZLIB_CHUNK_SIZE 16384
#define MAX_HEADERS 64
#define MAX_HEADER_LEN 1024

// --- Response Templates ---
#define RESPONSE_TEMPLATE \
    "HTTP/1.1 200 OK\r\n" \
    "Content-Type: text/html\r\n" \
    "Connection: keep-alive\r\n" \
    "Content-Length: %ld\r\n" \
    "\r\n" \
    "%s"

#define ERROR_TEMPLATE(status, msg) \
    "HTTP/1.1 " status "\r\n" \
    "Content-Type: text/html\r\n" \
    "Connection: close\r\n\r\n" \
    "<html><head><title>" status "</title></head>" \
    "<body><h1>" status "</h1><p>" msg "</p></body></html>\r\n"

typedef enum
{
    REQUEST_PROCESSED_OK,     // Successfully processed one request
    NEED_MORE_DATA,           // Parsed partial request, need more data from socket
    REQUEST_PARSE_ERROR,      // Malformed request or headers
    REQUEST_PROCESS_ERROR,    // Error during file handling, compression, or sending response
    CLIENT_CONNECTION_CLOSED, // Client closed connection (read returned 0) and buffer is handled
    BUFFER_EMPTY              // Ring buffer was empty, nothing to process
} RequestStatus;

// Error responses
extern const char *BAD_REQUEST_400;
extern const char *NOT_FOUND_404;
extern const char *NOT_IMPLEMENTED_501;
extern const char *PAYLOAD_TOO_LARGE_413;
extern const char *HEADER_FIELDS_TOO_LARGE_431;
extern const char *SUPPORTED_METHODS[];
extern const int SUPPORTED_METHOD_COUNT;

// --- Data Structures ---
typedef struct {
    char method[16];
    char path[1024];
    char headers[MAX_HEADERS][2][256]; // Header name, Header value
    int header_count;
    int keep_alive;
    int accepts_gzip;
} HTTPRequest;

// Return codes for the state machine
typedef enum {
    REQ_OK,             // Request fully processed
    REQ_NEED_DATA,      // Not enough data for headers yet
    REQ_FATAL_ERROR,    // Protocol error, close connection
    REQ_CLIENT_CLOSED   // Client disconnected gracefully
} ProcessResult;

struct BufferPool; 

// --- Function Prototypes (Interface) ---
void handle_client(int client_socket, BufferPool *bp);
int create_server_socket(void);
void send_error_response(int client_socket, const char *response);
int method_is_supported(const char *method);
void *worker_thread_function(void *arg);
void print_http_request(const HTTPRequest *req);
void sigchld_handler(int sig);

typedef enum {
    STATE_READING_REQUEST,
    STATE_PROCESSING,
    STATE_SENDING_HEADER,
    STATE_SENDING_BODY,
    STATE_CLOSING
} ConnState;

typedef struct {
    int socket_fd;
    ConnState state;
    
    // Buffers (Your RingBuffers go here)
    RingBuffer *in_buffer;
    RingBuffer *out_buffer;
    
    // Request State
    HTTPRequest current_request;
    int file_fd;         // File we are reading from
    off_t file_offset;   // How much we've read/sent
    long file_size;
    
    // Timeout tracking
    time_t last_activity;
} Connection;

// --- Ring Buffer Structure ---


// --- Ring Buffer Function Prototypes ---

#endif // HTTP_SERVER_H