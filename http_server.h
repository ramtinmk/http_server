// http_server.h
#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

// --- Configuration and Constants ---
#define PORT 8080
#define BACKLOG 10
#define BUFFER_SIZE 4096

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
    char method[8];
    char path[1024];
    char headers[32][2][256]; // [header_count][key/value]
    int header_count;
    int keep_alive;
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


#endif // HTTP_SERVER_H