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

#include "ring_buffer.c"

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


// --- Data Structures ---
typedef struct {
    char method[16];
    char path[1024];
    char headers[32][2][256]; // Header name, Header value
    int header_count;
    int keep_alive;
    int accepts_gzip; // <-- Add this flag
} HTTPRequest;

// --- Function Prototypes (Interface) ---
void handle_client(int client_socket);
int create_server_socket(void);
void send_error_response(int client_socket, const char *response);
int method_is_supported(const char *method);

// --- External Error Responses (Optional to put in header if test.c needs them directly) ---
extern const char *BAD_REQUEST_400;
extern const char *NOT_FOUND_404;
extern const char *NOT_IMPLEMENTED_501;

// Forward declaration
typedef struct ThreadPool ThreadPool;
typedef struct Task Task;

struct Task {
    int client_socket;
    // ... any other task specific data ...
    struct Task *next; // For linked list queue (or use array/deque)
};

struct ThreadPool {
    int pool_size;
    pthread_t *threads;        // Array of thread IDs
    Task *task_queue_head;     // Head of the task queue (if linked list)
    Task *task_queue_tail;     // Tail of the task queue (if linked list)
    pthread_mutex_t queue_mutex; // Mutex for task queue access
    pthread_cond_t queue_cond;   // Condition variable for task queue
    int shutdown;              // Flag to indicate shutdown
    // ... other thread pool management data ...
};

// --- Ring Buffer Structure ---


// --- Ring Buffer Function Prototypes ---

#endif // HTTP_SERVER_H