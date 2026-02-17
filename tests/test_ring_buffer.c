#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h> // Added for timing
#include "ring_buffer.h"
#include "test_utils.h"
#include "test_ring_buffer.h"

// --- Helper Functions ---
void verify_buffer_content(RingBuffer *rb, const char *expected) {
    char buf[1024];
    memset(buf, 0, sizeof(buf));
    size_t len = strlen(expected);
    
    size_t peeked = ring_buffer_peek(rb, buf, len);
    TEST_ASSERT(peeked == len);
    TEST_ASSERT(strncmp(buf, expected, len) == 0);
}

// --- Functional Test Cases ---

void test_create_and_destroy() {
    RingBuffer *rb = ring_buffer_create(100);
    TEST_ASSERT(rb != NULL);
    TEST_ASSERT(ring_buffer_get_capacity(rb) == 100);
    TEST_ASSERT(ring_buffer_get_size(rb) == 0);
    TEST_ASSERT(ring_buffer_is_empty(rb) == 1);
    
    ring_buffer_free(rb);
}

void test_basic_read_write() {
    RingBuffer *rb = ring_buffer_create(10);
    const char *data = "Hello";
    
    printf("       -> Writing 'Hello'...\n");
    size_t written = ring_buffer_write(rb, data, 5);
    TEST_ASSERT(written == 5);
    TEST_ASSERT(ring_buffer_get_size(rb) == 5);
    
    printf("       -> Reading back...\n");
    char buf[10];
    size_t read = ring_buffer_read(rb, buf, 5);
    buf[5] = '\0';
    
    TEST_ASSERT(read == 5);
    TEST_ASSERT(strcmp(buf, "Hello") == 0);
    TEST_ASSERT(ring_buffer_is_empty(rb));
    
    ring_buffer_free(rb);
}

void test_wrap_around() {
    RingBuffer *rb = ring_buffer_create(5);
    
    printf("       -> Step 1: Fill buffer [12345]\n");
    ring_buffer_write(rb, "12345", 5);
    TEST_ASSERT(ring_buffer_is_full(rb));
    
    printf("       -> Step 2: Read 2 bytes (creates gap at start)\n");
    char tmp[5];
    ring_buffer_read(rb, tmp, 2);
    
    printf("       -> Step 3: Write 'AB' (wraps to start indices)\n");
    ring_buffer_write(rb, "AB", 2);
    
    TEST_ASSERT(ring_buffer_get_size(rb) == 5);
    
    printf("       -> Step 4: Verify logical order is '345AB'\n");
    char out[6];
    size_t read = ring_buffer_read(rb, out, 5);
    out[5] = '\0';
    
    TEST_ASSERT(read == 5);
    TEST_ASSERT(strcmp(out, "345AB") == 0);
    
    ring_buffer_free(rb);
}

void test_auto_resize_simple() {
    printf("       -> Initial capacity: 4\n");
    RingBuffer *rb = ring_buffer_create(4);
    
    printf("       -> Writing 6 bytes (overflows 4)...\n");
    const char *data = "123456";
    size_t written = ring_buffer_write(rb, data, 6);
    
    printf("       -> New capacity: %zu\n", ring_buffer_get_capacity(rb));
    
    TEST_ASSERT(written == 6);
    TEST_ASSERT(ring_buffer_get_capacity(rb) >= 6); 
    TEST_ASSERT(ring_buffer_get_size(rb) == 6);
    
    verify_buffer_content(rb, "123456");
    
    ring_buffer_free(rb);
}

void test_auto_resize_with_wrap() {
    RingBuffer *rb = ring_buffer_create(4);
    
    printf("       -> Setup: Create wrapped state [EF..CD]\n");
    ring_buffer_write(rb, "ABCD", 4); 
    char tmp[2];
    ring_buffer_read(rb, tmp, 2);     
    ring_buffer_write(rb, "EF", 2);   
    
    printf("       -> Trigger resize by writing 'G'...\n");
    ring_buffer_write(rb, "G", 1);
    
    printf("       -> Verifying linearization and order...\n");
    TEST_ASSERT(ring_buffer_get_size(rb) == 5);
    
    char out[6];
    ring_buffer_read(rb, out, 5);
    out[5] = '\0';
    
    TEST_ASSERT(strcmp(out, "CDEFG") == 0);
    
    ring_buffer_free(rb);
}

void test_peek() {
    RingBuffer *rb = ring_buffer_create(10);
    ring_buffer_write(rb, "PeekMe", 6);
    
    char buf[10];
    ring_buffer_peek(rb, buf, 4);
    buf[4] = '\0';
    
    TEST_ASSERT(strcmp(buf, "Peek") == 0);
    TEST_ASSERT(ring_buffer_get_size(rb) == 6); // Ensure data remains
    
    ring_buffer_free(rb);
}

void test_readline_basic() {
    RingBuffer *rb = ring_buffer_create(20);
    ring_buffer_write(rb, "Hello\nWorld\n", 12);
    
    char line[50];
    printf("       -> Reading first line...\n");
    char *res = ring_buffer_readline(rb, line, sizeof(line));
    
    TEST_ASSERT(res != NULL);
    TEST_ASSERT(strcmp(line, "Hello") == 0);
    
    printf("       -> Reading second line...\n");
    res = ring_buffer_readline(rb, line, sizeof(line));
    TEST_ASSERT(res != NULL);
    TEST_ASSERT(strcmp(line, "World") == 0);
    
    ring_buffer_free(rb);
}

void test_readline_crlf() {
    RingBuffer *rb = ring_buffer_create(20);
    printf("       -> Writing '...\\r\\n'...\n");
    ring_buffer_write(rb, "HTTP/1.1 200 OK\r\n", 17);
    
    char line[50];
    ring_buffer_readline(rb, line, sizeof(line));
    
    printf("       -> Verifying '\\r' was stripped...\n");
    TEST_ASSERT(strcmp(line, "HTTP/1.1 200 OK") == 0);
    
    ring_buffer_free(rb);
}

void test_readline_wrapped_newline() {
    printf("       -> Forcing '\\n' to physically wrap to index 0...\n");
    RingBuffer *rb = ring_buffer_create(6); 
    
    // Move pointers to 4
    ring_buffer_write(rb, "1234", 4);
    char tmp[5];
    ring_buffer_read(rb, tmp, 4);
    
    // Write "AB\n". A=[4], B=[5], \n=[0]
    ring_buffer_write(rb, "AB\n", 3);
    
    char line[10];
    char *res = ring_buffer_readline(rb, line, sizeof(line));
    
    TEST_ASSERT(res != NULL);
    TEST_ASSERT(strcmp(line, "AB") == 0);
    
    ring_buffer_free(rb);
}

void test_readline_truncation() {
    RingBuffer *rb = ring_buffer_create(20);
    ring_buffer_write(rb, "123456789\nNEXT", 14);
    
    printf("       -> Line length is 9, but buffer is 5...\n");
    char line[5]; 
    ring_buffer_readline(rb, line, sizeof(line));
    
    printf("       -> Read: '%s'\n", line);
    TEST_ASSERT(strcmp(line, "1234") == 0);
    
    printf("       -> Verifying remainder of line was discarded...\n");
    char buf[10];
    size_t len = ring_buffer_read(rb, buf, 10);
    buf[len] = '\0';
    
    TEST_ASSERT(strcmp(buf, "NEXT") == 0);
    
    ring_buffer_free(rb);
}

void test_readline_no_newline() {
    RingBuffer *rb = ring_buffer_create(10);
    ring_buffer_write(rb, "Incomplete", 10);
    
    printf("       -> Attempting to read line without '\\n'...\n");
    char line[20];
    char *res = ring_buffer_readline(rb, line, sizeof(line));
    
    TEST_ASSERT(res == NULL);
    TEST_ASSERT(ring_buffer_get_size(rb) == 10);
    
    ring_buffer_free(rb);
}

void test_reset() {
    RingBuffer *rb = ring_buffer_create(10);
    ring_buffer_write(rb, "Data", 4);
    
    printf("       -> Resetting buffer...\n");
    ring_buffer_reset(rb);
    
    TEST_ASSERT(ring_buffer_is_empty(rb));
    TEST_ASSERT(ring_buffer_get_size(rb) == 0);
    
    ring_buffer_free(rb);
}

// --- Efficiency Tests ---

void test_efficiency_readline_streaming() {
    // Scenario: Simulate continuous processing of HTTP headers.
    // We write a line, then immediately read it. This causes the ring buffer
    // to wrap around constantly, aggressively testing the "wrap logic" and memchr.
    
    RingBuffer *rb = ring_buffer_create(4096);
    const char *header = "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64)\r\n";
    size_t header_len = strlen(header);
    char read_buf[256];
    
    const int ITERATIONS = 1000000;
    
    printf("       -> Processing %d headers (~%zu MB)...\n", ITERATIONS, (header_len * ITERATIONS)/(1024*1024));
    
    clock_t start = clock();
    
    for(int i = 0; i < ITERATIONS; i++) {
        // Write line
        ring_buffer_write(rb, header, header_len);
        
        // Read line
        char *res = ring_buffer_readline(rb, read_buf, sizeof(read_buf));
        
        if (!res) {
            fprintf(stderr, "Efficiency test failed at iteration %d\n", i);
            exit(EXIT_FAILURE);
        }
    }
    
    clock_t end = clock();
    double time_spent = (double)(end - start) / CLOCKS_PER_SEC;
    double throughput = ((double)(header_len * ITERATIONS) / (1024.0 * 1024.0)) / time_spent;
    
    printf("       -> Time: %.3fs | Throughput: %.2f MB/s\n", time_spent, throughput);
    
    ring_buffer_free(rb);
}

void test_efficiency_bulk_resize() {
    // Scenario: Write 10MB of data into a small buffer. 
    // This forces multiple resize operations (malloc + memcpy linearization).
    
    size_t initial_cap = 128;
    size_t data_size = 10 * 1024 * 1024; // 10 MB
    
    RingBuffer *rb = ring_buffer_create(initial_cap);
    char *dummy_data = malloc(data_size);
    memset(dummy_data, 'A', data_size); // Fill with 'A'
    
    printf("       -> Writing 10MB to 128B buffer (forces resize)...\n");
    
    clock_t start = clock();
    
    size_t written = ring_buffer_write(rb, dummy_data, data_size);
    
    clock_t end = clock();
    double time_spent = (double)(end - start) / CLOCKS_PER_SEC;
    
    TEST_ASSERT(written == data_size);
    TEST_ASSERT(ring_buffer_get_size(rb) == data_size);
    TEST_ASSERT(ring_buffer_get_capacity(rb) >= data_size);
    
    printf("       -> Time: %.3fs | Final Capacity: %zu bytes\n", time_spent, ring_buffer_get_capacity(rb));
    
    free(dummy_data);
    ring_buffer_free(rb);
}

// --- Public Entry Point with Descriptions ---
void run_ring_buffer_tests(void) {
    printf("=== Ring Buffer Suite ===\n\n");
    
    // Logic Tests
    RUN_TEST(test_create_and_destroy,       "Verify creation, capacity, and cleanup");
    RUN_TEST(test_basic_read_write,         "Simple FIFO write and read operations");
    RUN_TEST(test_wrap_around,              "Write/Read logic when data wraps physical end");
    RUN_TEST(test_auto_resize_simple,       "Buffer expansion when writing > capacity");
    RUN_TEST(test_auto_resize_with_wrap,    "Resize + Linearization when data is wrapped");
    RUN_TEST(test_peek,                     "View data without advancing read pointer");
    RUN_TEST(test_readline_basic,           "Extract line ending with \\n");
    RUN_TEST(test_readline_crlf,            "Extract line ending with \\r\\n (strip \\r)");
    RUN_TEST(test_readline_wrapped_newline, "Handle \\n located at buffer start (wrap)");
    RUN_TEST(test_readline_truncation,      "Handle destination buffer smaller than line");
    RUN_TEST(test_readline_no_newline,      "Return NULL if no newline exists");
    RUN_TEST(test_reset,                    "Clear buffer state");

    // Performance Tests
    printf("\n--- Efficiency Tests ---\n");
    RUN_TEST(test_efficiency_readline_streaming, "Throughput: Stream 1M headers (Write/Read cycle)");
    RUN_TEST(test_efficiency_bulk_resize,        "Throughput: Bulk write causing exponential resize");
}