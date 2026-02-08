#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/select.h> // Required for select()
#include <sys/time.h>   // Required for struct timeval
#include "test_utils.h"
#include "server_test.h"

// --- Configuration ---
#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 8080
#define BUFFER_SIZE 4096

// --- Helper Structures ---
typedef struct {
    int thread_id;
    int client_socket;
    const char* request;
    char* response;
} ThreadArgs;

// --- Helper Functions (Internal) ---

static int create_and_connect_socket() {
    int client_socket;
    struct sockaddr_in server_addr;

    if ((client_socket = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("Socket creation failed");
        return -1;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    if (inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr) <= 0) {
        perror("Invalid address/ Address not supported");
        close(client_socket);
        return -1;
    }

    if (connect(client_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        // Only print if connection truly fails (server might be down)
        // perror("Connection failed"); 
        close(client_socket);
        return -1;
    }

    return client_socket;
}

// FIXED: Now loops to read full response (Headers + Body)
static char* send_http_request(int client_socket, const char* request) {
    // 1. Send Request
    if (send(client_socket, request, strlen(request), 0) == -1) {
        perror("Send failed");
        return NULL;
    }

    // 2. Prepare Dynamic Buffer
    size_t capacity = BUFFER_SIZE;
    size_t size = 0;
    char *response = malloc(capacity);
    if (!response) return NULL;
    response[0] = '\0';

    // 3. Read Loop
    while (1) {
        // Ensure we have space
        if (capacity - size < BUFFER_SIZE) {
            capacity *= 2;
            char *new_resp = realloc(response, capacity);
            if (!new_resp) { free(response); return NULL; }
            response = new_resp;
        }

        // Wait up to 100ms for data (Handle packet splitting)
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(client_socket, &readfds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000; // 0.1 seconds timeout

        // Only use select check AFTER the first read, or rely on it for all.
        // For simplicity: Check if data is available.
        int ret = select(client_socket + 1, &readfds, NULL, NULL, &tv);
        
        if (ret < 0) {
            perror("Select error");
            break; 
        } 
        if (ret == 0) {
            // Timeout: If we already have data, assume message is done (Keep-Alive case).
            // If we have 0 data, we might be waiting for server to start processing.
            if (size > 0) break;
            
            // If size is 0, we give it one more chance or treat as slow server?
            // For tests, let's treat timeout with size 0 as "No response yet, wait a bit more"
            // But to avoid infinite loop, we rely on a slightly larger initial wait implied by select.
            // If timeout happens at size 0, we return NULL or empty.
            if (size == 0) break; 
        }

        ssize_t bytes_received = recv(client_socket, response + size, capacity - size - 1, 0);

        if (bytes_received < 0) {
            perror("Receive failed");
            free(response);
            return NULL;
        } else if (bytes_received == 0) {
            // Connection closed by server
            break; 
        } else {
            size += bytes_received;
            response[size] = '\0';
            // Continue loop to see if more data comes immediately (e.g. Body after Headers)
        }
    }

    if (size == 0) {
        free(response);
        return NULL;
    }

    return response;
}

// Thread worker function
static void* multithread_worker(void* thread_arg) {
    ThreadArgs* args = (ThreadArgs*)thread_arg;
    args->response = send_http_request(args->client_socket, args->request);
    close(args->client_socket);
    return NULL;
}

// --- Test Cases ---

void test_server_connectivity() {
    int sock = create_and_connect_socket();
    TEST_ASSERT(sock != -1); 
    if(sock != -1) close(sock);
}

void test_home_page() {
    int client_socket = create_and_connect_socket();
    TEST_ASSERT(client_socket != -1);

    char request[] = "GET /home HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    char *response = send_http_request(client_socket, request);

    TEST_ASSERT(response != NULL);
    TEST_ASSERT(strstr(response, "HTTP/1.1 200 OK") != NULL);
    // This check failed before because body wasn't fully read:
    TEST_ASSERT(strstr(response, "<title>Home Page</title>") != NULL);
    
    free(response);
    close(client_socket);
}

void test_hello_page() {
    int client_socket = create_and_connect_socket();
    TEST_ASSERT(client_socket != -1);

    char request[] = "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    char *response = send_http_request(client_socket, request);

    TEST_ASSERT(response != NULL);
    TEST_ASSERT(strstr(response, "HTTP/1.1 200 OK") != NULL);
    TEST_ASSERT(strstr(response, "Greetings!") != NULL);

    free(response);
    close(client_socket);
}

void test_root_page_load() {
    int client_socket = create_and_connect_socket();
    TEST_ASSERT(client_socket != -1);

    char request[] = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    char *response = send_http_request(client_socket, request);

    TEST_ASSERT(response != NULL);
    TEST_ASSERT(strstr(response, "HTTP/1.1 200 OK") != NULL);
    
    free(response);
    close(client_socket);
}

void test_not_found_404() {
    int client_socket = create_and_connect_socket();
    TEST_ASSERT(client_socket != -1);

    char request[] = "GET /nonexistent_path_12345 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    char *response = send_http_request(client_socket, request);

    TEST_ASSERT(response != NULL);
    TEST_ASSERT(strstr(response, "HTTP/1.1 404 Not Found") != NULL);

    free(response);
    close(client_socket);
}

void test_not_implemented_501() {
    int client_socket = create_and_connect_socket();
    TEST_ASSERT(client_socket != -1);

    char request[] = "DELETE /index.html HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    char *response = send_http_request(client_socket, request);

    TEST_ASSERT(response != NULL);
    TEST_ASSERT(strstr(response, "HTTP/1.1 501") != NULL || strstr(response, "HTTP/1.1 405") != NULL);

    free(response);
    close(client_socket);
}

void test_keep_alive() {
    int client_socket = create_and_connect_socket();
    TEST_ASSERT(client_socket != -1);

    // 1. First Request
    char req1[] = "GET /home HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
    char *resp1 = send_http_request(client_socket, req1);
    
    TEST_ASSERT(resp1 != NULL);
    TEST_ASSERT(strstr(resp1, "HTTP/1.1 200 OK") != NULL);
    free(resp1);

    // 2. Second Request (Same Socket)
    char req2[] = "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    char *resp2 = send_http_request(client_socket, req2);
    
    TEST_ASSERT(resp2 != NULL);
    TEST_ASSERT(strstr(resp2, "HTTP/1.1 200 OK") != NULL);
    free(resp2);

    close(client_socket);
}

// --- Multithreaded Tests (Wrappers) ---

void test_multithread_load() {
    int num_threads = 10;
    pthread_t threads[num_threads];
    ThreadArgs args[num_threads];

    printf("       -> Spawning %d threads for concurrent requests...\n", num_threads);

    for (int i = 0; i < num_threads; i++) {
        args[i].thread_id = i;
        args[i].client_socket = create_and_connect_socket();
        
        TEST_ASSERT(args[i].client_socket != -1);
        
        args[i].request = "GET /home HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        args[i].response = NULL;

        if (pthread_create(&threads[i], NULL, multithread_worker, &args[i]) != 0) {
            close(args[i].client_socket);
            TEST_ASSERT(0 && "Thread creation failed");
        }
    }

    // Join and Verify
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
        
        TEST_ASSERT(args[i].response != NULL);
        if (args[i].response) {
            TEST_ASSERT(strstr(args[i].response, "HTTP/1.1 200 OK") != NULL);
            free(args[i].response);
        }
    }
}

void test_gzip_concurrency() {
    int num_threads = 5;
    pthread_t threads[num_threads];
    ThreadArgs args[num_threads];

    printf("       -> Spawning %d threads requesting GZIP...\n", num_threads);

    const char *gzip_req = "GET /home HTTP/1.1\r\nHost: localhost\r\nAccept-Encoding: gzip\r\nConnection: close\r\n\r\n";

    for (int i = 0; i < num_threads; i++) {
        args[i].thread_id = i;
        args[i].client_socket = create_and_connect_socket();
        TEST_ASSERT(args[i].client_socket != -1);
        
        args[i].request = gzip_req;
        args[i].response = NULL;

        pthread_create(&threads[i], NULL, multithread_worker, &args[i]);
    }

    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
        
        TEST_ASSERT(args[i].response != NULL);
        if (args[i].response) {
            TEST_ASSERT(strstr(args[i].response, "HTTP/1.1 200 OK") != NULL);
            
            int has_gzip = (strstr(args[i].response, "Content-Encoding: gzip") != NULL) || 
                           (strstr(args[i].response, "content-encoding: gzip") != NULL);
            
            TEST_ASSERT(has_gzip);
            free(args[i].response);
        }
    }
}

// --- Runner ---

void run_server_tests() {
    printf("=== General Server Suite ===\n");
    printf("Note: Ensure the server is running on %s:%d before starting tests.\n\n", SERVER_IP, SERVER_PORT);

    // Basic Connectivity
    RUN_TEST(test_server_connectivity, "Check if server is reachable");

    // Standard Endpoints
    RUN_TEST(test_root_page_load,   "GET / (Root)");
    RUN_TEST(test_home_page,        "GET /home (Check content)");
    RUN_TEST(test_hello_page,       "GET /hello (Check content)");

    // Error Handling
    RUN_TEST(test_not_found_404,    "GET /nonexistent (Expect 404)");
    RUN_TEST(test_not_implemented_501, "DELETE / (Expect 501/405)");

    // Advanced Features
    RUN_TEST(test_keep_alive,       "Keep-Alive: Multiple reqs on one socket");
    RUN_TEST(test_multithread_load, "Concurrency: 10 simultaneous requests");
    RUN_TEST(test_gzip_concurrency, "Concurrency: GZIP requests");
}