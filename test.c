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

void test_home_page() {
    int client_socket = create_and_connect_socket();
    if (client_socket == -1) return;

    char request[] = "GET /home HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    char *response = send_http_request(client_socket, request);

    if (response != NULL) {
        if (strstr(response, "HTTP/1.1 200 OK") != NULL &&
            strstr(response, "<title>Home Page</title>") != NULL &&
            strstr(response, "<h1>Welcome to the Home Page!</h1>") != NULL) {
            printf("Test /home Page: PASS\n");
        } else {
            printf("Test /home Page: FAIL - Incorrect content or status\n");
        }
        free(response);
    } else {
        printf("Test /home Page: FAIL - No response or error\n");
    }
    close(client_socket);
}

void test_hello_page() {
    int client_socket = create_and_connect_socket();
    if (client_socket == -1) return;

    char request[] = "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    char *response = send_http_request(client_socket, request);

    if (response != NULL) {
        if (strstr(response, "HTTP/1.1 200 OK") != NULL &&
            strstr(response, "<title>Hello Page</title>") != NULL &&
            strstr(response, "<h1>Greetings!</h1>") != NULL) {
            printf("Test /hello Page: PASS\n");
        } else {
            printf("Test /hello Page: FAIL - Incorrect content or status\n");
        }
        free(response);
    } else {
        printf("Test /hello Page: FAIL - No response or error\n");
    }
    close(client_socket);
}


void test_root_page() { // Testing root path, might serve home.html
    int client_socket = create_and_connect_socket();
    if (client_socket == -1) return;

    char request[] = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    char *response = send_http_request(client_socket, request);

    if (response != NULL) {
        if (strstr(response, "HTTP/1.1 200 OK") != NULL &&
            strstr(response, "<title>Home Page</title>") != NULL &&
            strstr(response, "<h1>Welcome to the Home Page!</h1>") != NULL) { // Expecting home.html content
            printf("Test / (root) Page: PASS\n");
        } else {
            printf("Test / (root) Page: FAIL - Incorrect content or status\n");
        }
        free(response);
    } else {
        printf("Test / (root) Page: FAIL - No response or error\n");
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

void test_keep_alive_connection() {
    int client_socket = create_and_connect_socket();
    if (client_socket == -1) return;

    char request1[] = "GET /home HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
    char *response1 = send_http_request(client_socket, request1);

    if (response1 == NULL || strstr(response1, "HTTP/1.1 200 OK") == NULL || strstr(response1, "<title>Home Page</title>") == NULL) {
        printf("Test Keep-Alive (Request 1 - /home): FAIL - Initial request failed or incorrect response\n");
        free(response1);
        close(client_socket);
        return;
    }
    free(response1); // Free response 1 memory

    char request2[] = "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
    char *response2 = send_http_request(client_socket, request2);

    if (response2 == NULL || strstr(response2, "HTTP/1.1 200 OK") == NULL || strstr(response2, "<title>Hello Page</title>") == NULL) {
        printf("Test Keep-Alive (Request 2 - /hello): FAIL - Second request failed or incorrect response\n");
        free(response2);
        close(client_socket);
        return;
    }
    free(response2); // Free response 2 memory


    char request3_close[] = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    char *response3_close = send_http_request(client_socket, request3_close);

    if (response3_close == NULL || strstr(response3_close, "HTTP/1.1 200 OK") == NULL) {
        printf("Test Keep-Alive (Request 3 - / with Connection: close): FAIL - Third request failed or incorrect response\n");
        free(response3_close);
        close(client_socket);
        return;
    }
    free(response3_close); // Free response 3 memory

    printf("Test Keep-Alive Connection: PASS - Multiple requests over same connection successful\n");
    close(client_socket); // Explicitly close socket after Keep-Alive test
}

int main() {
    printf("Starting HTTP Server Tests...\n");

    test_root_page();      // Test root path "/"
    test_home_page();      // Test /home
    test_hello_page();     // Test /hello
    test_not_found_404();
    test_not_implemented_501();
    test_keep_alive_connection();
    // test_ok_response(); // You can comment out the original root test if root now serves home.html

    printf("HTTP Server Tests Completed.\n");
    return 0;
}