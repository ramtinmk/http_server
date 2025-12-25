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
    "Connection: keep-alive\r\n\r\n" \
    "<html><head><title>" status "</title></head>" \
    "<body><h1>" status "</h1><p>" msg "</p></body></html>\r\n"
// Add this enum definition near the top or in http_server.h if preferred
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
extern const char *SUPPORTED_METHODS[];
extern const int SUPPORTED_METHOD_COUNT;


// --- Data Structures ---
typedef struct {
    char method[16];
    char path[1024];
    char headers[32][2][256]; // Header name, Header value
    int header_count;
    int keep_alive;
    int accepts_gzip; // <-- Add this flag
} HTTPRequest;

struct BufferPool; 

// --- Function Prototypes (Interface) ---
void handle_client(int client_socket, BufferPool *bp);
int create_server_socket(void);
void send_error_response(int client_socket, const char *response);
int method_is_supported(const char *method);
void *worker_thread_function(void *arg);
static RequestStatus process_single_request(int client_socket, RingBuffer *request_rb, HTTPRequest *request, int *keep_alive_connection, int client_closed_flag);

// static void parse_header_line(char *line, HTTPRequest *req);
void print_http_request(const HTTPRequest *req);
// static void parse_request_line(char *line, HTTPRequest *req);
int create_server_socket(void);
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