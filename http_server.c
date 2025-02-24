/*
 * Simple HTTP Server for Educational Purposes
 *
 * This implementation demonstrates basic socket programming
 * and HTTP protocol handling. Not recommended for production use.
 */

 #include <stdio.h>
 #include <stdlib.h>
 #include <string.h>
 #include <unistd.h>
 #include <sys/socket.h>
 #include <netinet/in.h>
 #include <arpa/inet.h>
 #include <sys/wait.h>
 #include <signal.h>
//  #include <signal.h>
 
#define PORT 8080
#define BACKLOG 10
#define BUFFER_SIZE 4096
#define RESPONSE_TEMPLATE \
"HTTP/1.1 200 OK\r\n" \
"Content-Type: text/html\r\n" \
"Connection: close\r\n" \
"Content-Length: %ld\r\n" /* Add Content-Length header */ \
"\r\n" \
"%s" /* Placeholder for HTML content */


#define ERROR_TEMPLATE(status, msg) \
    "HTTP/1.1 " status "\r\n" \
    "Content-Type: text/html\r\n" \
    "Connection: close\r\n\r\n" \
    "<html><head><title>" status "</title></head>" \
    "<body><h1>" status "</h1><p>" msg "</p></body></html>\r\n"


 typedef struct {
     char method[8];
     char path[1024];
     char headers[32][2][256]; // [header_count][key/value]
     int header_count;
 } HTTPRequest;
 
 // Function declarations
 void handle_client(int client_socket);
 int create_server_socket(void);
 void sigchld_handler(int sig);
 static void parse_request_line(char *line, HTTPRequest *req);
 static void parse_header_line(char *line, HTTPRequest *req);
 


// Error responses
static const char *BAD_REQUEST_400 = ERROR_TEMPLATE("400 Bad Request", "Malformed request syntax");
static const char *NOT_FOUND_404 = ERROR_TEMPLATE("404 Not Found", "The requested resource was not found");
static const char *NOT_IMPLEMENTED_501 = ERROR_TEMPLATE("501 Not Implemented", "HTTP method not supported");

// Supported methods
const char *SUPPORTED_METHODS[] = {"GET", "HEAD"};
const int SUPPORTED_METHOD_COUNT = 2;

// Helper functions
void send_error_response(int client_socket, const char *response);
int method_is_supported(const char *method);
int validate_request(HTTPRequest *req);


 int main() {
     int server_socket, client_socket;
     struct sockaddr_in client_addr;
     socklen_t addr_size = sizeof(client_addr);
     
     // Set up SIGCHLD handler to prevent zombie processes
     struct sigaction sa;
     sa.sa_handler = sigchld_handler;
     sigemptyset(&sa.sa_mask);
     sa.sa_flags = SA_RESTART;
     if (sigaction(SIGCHLD, &sa, NULL) == -1) {
         perror("sigaction");
         exit(EXIT_FAILURE);
     }
 
     // Create server socket
     server_socket = create_server_socket();
     printf("Server listening on port %d...\n", PORT);
 
     while(1) {
         // Accept incoming connection
         client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &addr_size);
         if (client_socket == -1) {
             perror("accept");
             continue;
         }
 
         // Fork child process to handle client
         pid_t pid = fork();
         if (pid == 0) { // Child process
             close(server_socket);
             printf("Client connected: %s:%d\n", 
                    inet_ntoa(client_addr.sin_addr), 
                    ntohs(client_addr.sin_port));
             
             handle_client(client_socket);
             
             close(client_socket);
             exit(EXIT_SUCCESS);
         } else if (pid > 0) { // Parent process
             close(client_socket);
         } else {
             perror("fork");
         }
     }
 
     close(server_socket);
     return 0;
 }
 
 static void parse_request_line(char *line, HTTPRequest *req) {
     char *method_end = strchr(line, ' ');
     if (!method_end) return;
     *method_end = '\0';
     strncpy(req->method, line, sizeof(req->method) - 1);
     req->method[sizeof(req->method) - 1] = '\0';
     
     char *path_start = method_end + 1;
     char *path_end = strchr(path_start, ' ');
     if (!path_end) return;
     *path_end = '\0';
     strncpy(req->path, path_start, sizeof(req->path) - 1);
     req->path[sizeof(req->path) - 1] = '\0';
 }
 
 static void parse_header_line(char *line, HTTPRequest *req) {
     if (req->header_count >= 32) return; // Max headers reached
     char *colon = strchr(line, ':');
     if (!colon) return;
     *colon = '\0';
     char *value = colon + 1;
     
     // Trim leading whitespace from value
     while (*value == ' ' || *value == '\t') value++;
     
     strncpy(req->headers[req->header_count][0], line, 255);
     req->headers[req->header_count][0][255] = '\0';
     strncpy(req->headers[req->header_count][1], value, 255);
     req->headers[req->header_count][1][255] = '\0';
     req->header_count++;
 }
 
void print_http_request(const HTTPRequest *req) {
    printf("Parsed HTTP Request:\n");
    printf("  Method: %s\n", req->method);
    printf("  Path: %s\n", req->path);
    printf("  Headers:\n");
    for (int i = 0; i < req->header_count; i++) {
        printf("    %s: %s\n", req->headers[i][0], req->headers[i][1]);
    }
}
void handle_client(int client_socket) {
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;

    bytes_read = read(client_socket, buffer, BUFFER_SIZE - 1);
    if (bytes_read < 0) {
        perror("read");
        return;
    }
    buffer[bytes_read] = '\0';

    HTTPRequest request = {0};
    char *ptr = buffer;
    char *end = buffer + bytes_read;

    // Parse request line
    char *line_end = strstr(ptr, "\r\n");
    if (!line_end) {
        send_error_response(client_socket, BAD_REQUEST_400);
        return;
    }
    *line_end = '\0';
    parse_request_line(ptr, &request);
    ptr = line_end + 2;

    // Validate method
    if (!method_is_supported(request.method)) {
        send_error_response(client_socket, NOT_IMPLEMENTED_501);
        return;
    }

    // Parse headers (same as before)
    while (ptr < end) {
        line_end = strstr(ptr, "\r\n");
        if (!line_end) break;
        *line_end = '\0';

        if (ptr == line_end) { // End of headers
            ptr += 2;
            break;
        }
        parse_header_line(ptr, &request);
        ptr = line_end + 2;
    }

    // Validate Host header (same as before)
    int has_host = 0;
    for (int i = 0; i < request.header_count; i++) {
        if (strcasecmp(request.headers[i][0], "Host") == 0) {
            has_host = 1;
            break;
        }
    }
    if (!has_host) {
        send_error_response(client_socket, BAD_REQUEST_400);
        return;
    }

    char filepath[1024];
    char response_body[BUFFER_SIZE];
    long file_size = 0;
    FILE *html_file = NULL;
    int status_code = 200; // Default OK

    if (strcmp(request.path, "/home") == 0) {
        snprintf(filepath, sizeof(filepath), "home.html"); // File in same directory
    } else if (strcmp(request.path, "/hello") == 0) {
        snprintf(filepath, sizeof(filepath), "hello.html"); // File in same directory
    } else if (strcmp(request.path, "/") == 0) { // Optionally serve a default root page?
        snprintf(filepath, sizeof(filepath), "home.html"); // Or a different default, or 404
    }
     else {
        send_error_response(client_socket, NOT_FOUND_404);
        return; // Early return for 404
    }

    html_file = fopen(filepath, "r");
    if (html_file == NULL) {
        perror("fopen");
        send_error_response(client_socket, NOT_FOUND_404); // File not found is 404
        return;
    }

    // Get file size
    fseek(html_file, 0, SEEK_END);
    file_size = ftell(html_file);
    fseek(html_file, 0, SEEK_SET);

    if (file_size > sizeof(response_body) -1 ) {
        fprintf(stderr, "File too large to fit in buffer: %s\n", filepath);
        fclose(html_file);
        send_error_response(client_socket, NOT_FOUND_404); // Or 500 Internal Server Error?
        return;
    }

    // Read file content
    size_t bytes_read_from_file = fread(response_body, 1, file_size, html_file);
    if (bytes_read_from_file != file_size) {
        perror("fread");
        fclose(html_file);
        send_error_response(client_socket, NOT_FOUND_404); // Or 500 Internal Server Error?
        return;
    }
    response_body[bytes_read_from_file] = '\0'; // Null terminate

    fclose(html_file);


    char response[BUFFER_SIZE];
    snprintf(response, BUFFER_SIZE, RESPONSE_TEMPLATE, file_size, response_body);


    // Send response
    if (write(client_socket, response, strlen(response)) < 0) {
        perror("write");
    }
}
 int create_server_socket(void) {
     int server_socket;
     struct sockaddr_in server_addr;
 
     // Create socket
     if ((server_socket = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
         perror("socket");
         exit(EXIT_FAILURE);
     }
 
     // Set socket options
     int opt = 1;
     if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
         perror("setsockopt");
         exit(EXIT_FAILURE);
     }
 
     // Configure server address
     server_addr.sin_family = AF_INET;
     server_addr.sin_port = htons(PORT);
     server_addr.sin_addr.s_addr = INADDR_ANY;
 
     // Bind socket
     if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
         perror("bind");
         exit(EXIT_FAILURE);
     }
 
     // Start listening
     if (listen(server_socket, BACKLOG) == -1) {
         perror("listen");
         exit(EXIT_FAILURE);
     }
 
     return server_socket;
 }
 
void sigchld_handler(int sig) {
     (void)sig; // Silence unused parameter warning
     while (waitpid(-1, NULL, WNOHANG) > 0);
 }
 // New helper functions
void send_error_response(int client_socket, const char *response) {
    if (write(client_socket, response, strlen(response)) < 0) {
        perror("write error response");
    }
}

int method_is_supported(const char *method) {
    for (int i = 0; i < SUPPORTED_METHOD_COUNT; i++) {
        if (strcmp(method, SUPPORTED_METHODS[i]) == 0) {
            return 1;
        }
    }
    return 0;
}
