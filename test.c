// test.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <pthread.h>


#define SERVER_IP "127.0.0.1" // Loopback address
#define SERVER_PORT 8080
#define BUFFER_SIZE 2048

// --- Function Prototypes ---
void* multithread_request_handler(void* thread_arg);
char* send_http_request(int client_socket, const char* request);
int create_and_connect_socket();
void test_home_page();
void test_hello_page();
void test_root_page();
void test_not_found_404();
void test_not_implemented_501();
void test_keep_alive_connection();
int test_multithread_handling(int num_threads);
int main();

typedef struct {
    int thread_id;
    int client_socket;
    const char *request;
    char *response;
    int requests_per_thread; // Number of requests this thread should send
    int successful_requests; // Counter for successful requests in this thread
} ThreadArgs;

// Function executed by each thread in multithreaded test
void* multithread_request_handler(void* thread_arg) {
    ThreadArgs* args = (ThreadArgs*)thread_arg;
    args->response = send_http_request(args->client_socket, args->request);

    if (args->response != NULL) {
        // Basic check, you can add more specific checks if needed for multithreading test
        if (strstr(args->response, "HTTP/1.1 200 OK") != NULL) {
            printf("Thread %d: Request successful (basic check).\n", args->thread_id);
        } else {
            printf("Thread %d: Request FAIL - Incorrect status code (basic check).\n", args->thread_id);
        }
    } else {
        printf("Thread %d: Request FAIL - No response or error.\n", args->thread_id);
    }

    close(args->client_socket);
    pthread_exit(NULL);
    return NULL;
}

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

    char request[] = "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
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


void test_root_page(int test_number) { // Testing root path, might serve home.html
    int count = 0;
    for (int i=0;i<test_number;i++)
    {
    int client_socket = create_and_connect_socket();
    
    if (client_socket == -1) return;

    
    char request[] = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
    char *response = send_http_request(client_socket, request);

    if (response != NULL) {
        if (strstr(response, "HTTP/1.1 200 OK") != NULL &&
            strstr(response, "<title>Home Page</title>") != NULL &&
            strstr(response, "<h1>Welcome to the Home Page!</h1>") != NULL) { // Expecting home.html content
            printf("Test / (root) Page: PASS\n");
        } else {
            printf("Test / (root) Page: FAIL - Incorrect content or status\n");
            count++;
        }
        free(response);
    } else {
        printf("Test / (root) Page: FAIL - No response or error\n");
        count++;
    }
    close(client_socket);

    sleep(0.01);

}
    

    
    printf("%d number failed",count);
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

int test_multithread_handling(int num_threads) {
    pthread_t threads[num_threads];
    ThreadArgs thread_args[num_threads];

    printf("Starting Multithread Test with %d threads...\n", num_threads);

    for (int i = 0; i < num_threads; i++) {
        int client_socket = create_and_connect_socket();
        if (client_socket == -1) {
            fprintf(stderr, "Failed to create client socket for thread %d, test aborted.\n", i);
            return -1;
        }

        thread_args[i].thread_id = i;
        thread_args[i].client_socket = client_socket;
        thread_args[i].request = "GET /home HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"; // Example request
        thread_args[i].response = NULL; // Initialize response to NULL

        if (pthread_create(&threads[i], NULL, multithread_request_handler, &thread_args[i]) != 0) {
            perror("pthread_create");
            close(client_socket); // Close socket if thread creation fails
            fprintf(stderr, "Failed to create thread %d, test may be incomplete.\n", i);
            // Continue to next iteration to try and create remaining threads (for better cleanup)
        }
    }

    // Wait for all threads to complete
    for (int i = 0; i < num_threads; i++) {
        if (pthread_join(threads[i], NULL) != 0) {
            perror("pthread_join");
        }
    }

    int pass_count = 0;
    for (int i = 0; i < num_threads; i++) {
        if (thread_args[i].response != NULL && strstr(thread_args[i].response, "HTTP/1.1 200 OK") != NULL) {
            pass_count++;
            free(thread_args[i].response); // Free response memory
        }
    }

    if (pass_count == num_threads) {
        printf("Multithread Test with %d threads: PASS - All threads received OK responses.\n", num_threads);
        return 1;
    } else {
        printf("Multithread Test with %d threads: FAIL - %d/%d threads failed or received incorrect responses.\n", num_threads, num_threads - pass_count, num_threads);
        return 0;
    }
}

int test_multithread_gzip_handling(int num_threads) {
    pthread_t threads[num_threads];
    ThreadArgs thread_args[num_threads];

    printf("Starting Multithread GZIP Test with %d threads...\n", num_threads);

    // --- Request that asks for gzip encoding ---
    const char *gzip_request = "GET /home HTTP/1.1\r\n"
                               "Host: localhost\r\n"
                               "Accept-Encoding: gzip\r\n" // Ask for gzip
                               "Connection: close\r\n"
                               "\r\n";

    int threads_created = 0; // Keep track of successfully created threads for joining
    for (int i = 0; i < num_threads; i++) {
        int client_socket = create_and_connect_socket();
        if (client_socket == -1) {
            fprintf(stderr, "GZIP TEST: Failed to create client socket for thread %d, test aborted.\n", i);
            // Cleanup previously created threads before returning
            for (int j = 0; j < threads_created; j++) {
                 if (thread_args[j].client_socket != -1) { // Check if socket was valid before thread creation attempt
                    // Join might be problematic if thread creation failed, but attempt cleanup
                     pthread_join(threads[j], NULL); // Ignore join error here
                     // Ensure socket associated with successfully created threads is closed within handler or join loop
                 }
            }
            return -1; // Indicate test failure due to setup issue
        }

        thread_args[i].thread_id = i;
        thread_args[i].client_socket = client_socket; // Store socket before creating thread
        thread_args[i].request = gzip_request;      // Use the gzip request
        thread_args[i].response = NULL;               // Initialize response to NULL

        if (pthread_create(&threads[i], NULL, multithread_request_handler, &thread_args[i]) != 0) {
            perror("pthread_create (GZIP TEST)");
            close(client_socket); // Close socket if thread creation fails
            thread_args[i].client_socket = -1; // Mark socket as closed/invalid for this arg
            fprintf(stderr, "GZIP TEST: Failed to create thread %d, test may be incomplete.\n", i);
            // Do not increment threads_created
            // Continue to allow joining threads that *were* created
        } else {
            threads_created++; // Increment only if thread creation succeeded
        }
    }

    // Wait for all successfully created threads to complete
    printf("GZIP TEST: Waiting for %d threads to complete...\n", threads_created);
    for (int i = 0; i < threads_created; i++) {
         // Find the correct thread/args index. This assumes indices match,
         // which might be wrong if creation failed mid-loop. A safer approach
         // might be needed if failures are common or order matters significantly.
         // For simplicity, assume the first 'threads_created' elements correspond.
        if (thread_args[i].client_socket != -1) { // Only join threads we think were successfully created
             if (pthread_join(threads[i], NULL) != 0) {
                perror("pthread_join (GZIP TEST)");
            }
        }
        // The socket should be closed by the handler or here after join if handler doesn't
         if (thread_args[i].client_socket != -1) {
             // close(thread_args[i].client_socket); // Ensure socket is closed (might be redundant if handler closes)
             // The provided handler likely closes it, so commenting out here.
         }
    }
    printf("GZIP TEST: All threads joined.\n");


    int pass_count = 0;
    for (int i = 0; i < threads_created; i++) {
        // Check only args corresponding to successfully created threads
        if (thread_args[i].client_socket == -1) continue; // Skip args for threads that failed creation

        printf("GZIP TEST: Analyzing response for thread %d:\n", i);
        if (thread_args[i].response != NULL) {
            // Print first few lines for debugging if needed
            // char head_buf[200];
            // strncpy(head_buf, thread_args[i].response, 199);
            // head_buf[199] = '\0';
            // printf("----\n%s\n----\n", head_buf);

            // --- Verify GZIP-specific headers ---
            int ok_found = (strstr(thread_args[i].response, "HTTP/1.1 200 OK") != NULL);
            // Use strcasestr if available for case-insensitivity, otherwise use strstr
            int gzip_encoding_found = (strstr(thread_args[i].response, "Content-Encoding: gzip") != NULL || strstr(thread_args[i].response, "content-encoding: gzip") != NULL) ;
            int chunked_encoding_found = (strstr(thread_args[i].response, "Transfer-Encoding: chunked") != NULL || strstr(thread_args[i].response, "transfer-encoding: chunked") != NULL);

            if (ok_found && gzip_encoding_found && chunked_encoding_found) {
                printf("  Thread %d: PASS (200 OK, gzip, chunked headers found)\n", i);
                pass_count++;
            } else {
                printf("  Thread %d: FAIL (Headers incorrect: OK=%d, GzipEnc=%d, ChunkedEnc=%d)\n",
                       i, ok_found, gzip_encoding_found, chunked_encoding_found);
            }
            free(thread_args[i].response); // Free response memory
        } else {
             printf("  Thread %d: FAIL (Response was NULL)\n", i);
        }
    }

    printf("--- GZIP Test Summary ---\n");
    if (pass_count == threads_created && threads_created == num_threads) {
        printf("Multithread GZIP Test with %d threads: PASS - All threads created and received correct GZIP headers.\n", num_threads);
        return 1; // Success
    } else {
        printf("Multithread GZIP Test with %d threads: FAIL - %d/%d expected responses failed verification (or %d threads failed creation).\n",
               num_threads, threads_created - pass_count, threads_created, num_threads - threads_created);
        return 0; // Failure
    }
}

int main() {
    printf("Starting HTTP Server Tests...\n");

    // test_root_page(1000);
    // test_home_page();
    // test_hello_page();
    // test_not_found_404();
    // test_not_implemented_501();
    // test_keep_alive_connection();

    // // --- Multithreaded Tests ---
    // test_multithread_handling(1);     // Test with 1 thread (for baseline)
    // test_multithread_handling(4);     // Test with thread pool size threads
    // test_multithread_handling(10);    // Test with more threads than pool size
    int res = 0 ;
    for (int i=0;i<1000;i++){
     res+=test_multithread_handling(16);

    }    // Test with significantly more threads (stress test - adjust if needed)
    printf("%d",res);
    printf("HTTP Server Tests Completed.\n");
    return 0;
}