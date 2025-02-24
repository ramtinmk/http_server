// test.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#define SERVER_IP "127.0.0.1" // Loopback address
#define SERVER_PORT 8080
#define BUFFER_SIZE 2048

// Function to send HTTP request and receive response
char* send_http_request(int client_socket, const char* request) {
    char buffer[BUFFER_SIZE] = {0};
    ssize_t bytes_received;

    // Send HTTP request
    if (send(client_socket, request, strlen(request), 0) == -1) {
        perror("send");
        return NULL; // Indicate error
    }
    printf("HTTP request sent:\n%s", request);

    // Receive response from server
    bytes_received = recv(client_socket, buffer, BUFFER_SIZE - 1, 0);
    if (bytes_received < 0) {
        perror("recv");
        return NULL; // Indicate error
    } else if (bytes_received == 0) {
        printf("Connection closed by server.\n");
        return NULL; // Indicate closed connection
    } else {
        buffer[bytes_received] = '\0';
        printf("\nHTTP response received:\n%s\n", buffer);
        char *response_copy = strdup(buffer); // Allocate memory to return response string
        if (response_copy == NULL) {
            perror("strdup");
            return NULL;
        }
        return response_copy;
    }
}

int create_and_connect_socket() {
    int client_socket;
    struct sockaddr_in server_addr;

    // Create socket
    if ((client_socket = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("socket");
        return -1; // Indicate socket creation error
    }

    // Configure server address
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    if (inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(client_socket);
        return -1; // Indicate address conversion error
    }

    // Connect to the server
    if (connect(client_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("connect");
        close(client_socket);
        return -1; // Indicate connection error
    }

    printf("Connected to server %s:%d\n", SERVER_IP, SERVER_PORT);
    return client_socket;
}

void test_ok_response() {
    int client_socket = create_and_connect_socket();
    if (client_socket == -1) return;

    char request[] = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    char *response = send_http_request(client_socket, request);

    if (response != NULL) {
        if (strstr(response, "HTTP/1.1 200 OK") != NULL) {
            printf("Test OK Response: PASS\n");
        } else {
            printf("Test OK Response: FAIL - Incorrect status code\n");
        }
        free(response); // Free allocated memory for response
    } else {
        printf("Test OK Response: FAIL - No response received or error\n");
    }
    close(client_socket);
}


void test_not_found_404() {
    int client_socket = create_and_connect_socket();
    if (client_socket == -1) return;

    // Request a non-existent path
    char request[] = "GET /nonexistent_path HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    char *response = send_http_request(client_socket, request);

    if (response != NULL) {
        if (strstr(response, "HTTP/1.1 404 Not Found") != NULL) {
            printf("Test 404 Not Found: PASS\n");
        } else {
            printf("Test 404 Not Found: FAIL - Incorrect status code\n");
        }
        free(response); // Free allocated memory for response
    } else {
        printf("Test 404 Not Found: FAIL - No response received or error\n");
    }
    close(client_socket);
}

void test_not_implemented_501() {
    int client_socket = create_and_connect_socket();
    if (client_socket == -1) return;

    // Use a method not supported (POST)
    char request[] = "POST / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    char *response = send_http_request(client_socket, request);

    if (response != NULL) {
        if (strstr(response, "HTTP/1.1 501 Not Implemented") != NULL) {
            printf("Test 501 Not Implemented: PASS\n");
        } else {
            printf("Test 501 Not Implemented: FAIL - Incorrect status code\n");
        }
        free(response); // Free allocated memory for response
    } else {
        printf("Test 501 Not Implemented: FAIL - No response received or error\n");
    }
    close(client_socket);
}

int main() {
    printf("Starting HTTP Server Tests...\n");

    test_ok_response();
    test_not_found_404();
    test_not_implemented_501();

    printf("HTTP Server Tests Completed.\n");
    return 0;
}