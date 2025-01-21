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

#define PORT 8080
#define BACKLOG 10
#define BUFFER_SIZE 2048
#define RESPONSE_TEMPLATE "HTTP/1.1 200 OK\r\n" \
                          "Content-Type: text/html\r\n" \
                          "Connection: close\r\n" \
                          "\r\n" \
                          "<html><head><title>Test Server</title></head>" \
                          "<body><h1>Educational HTTP Server</h1>" \
                          "<p>Requested path: %s</p>" \
                          "<p>Method: %s</p>" \
                          "</body></html>\r\n"

// Function declarations
void handle_client(int client_socket);
int create_server_socket(void);
void sigchld_handler(int sig);

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

void handle_client(int client_socket) {
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;
    
    // Read client request
    bytes_read = read(client_socket, buffer, BUFFER_SIZE - 1);
    if (bytes_read < 0) {
        perror("read");
        return;
    }
    buffer[bytes_read] = '\0';

    // Parse request (simple version)
    char method[16], path[256], protocol[16];
    sscanf(buffer, "%s %s %s", method, path, protocol);

    // Prepare response
    char response[BUFFER_SIZE];
    snprintf(response, BUFFER_SIZE, RESPONSE_TEMPLATE, path, method);
    
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