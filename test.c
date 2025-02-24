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

// ... (create_and_connect_socket, send_http_request functions - keep them the same) ...


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


// ... (test_ok_response, test_not_found_404, test_not_implemented_501 - keep them) ...

int main() {
    printf("Starting HTTP Server Tests...\n");

    test_root_page();      // Test root path "/"
    test_home_page();      // Test /home
    test_hello_page();     // Test /hello
    test_not_found_404();
    test_not_implemented_501();
    // test_ok_response(); // You can comment out the original root test if root now serves home.html

    printf("HTTP Server Tests Completed.\n");
    return 0;
}