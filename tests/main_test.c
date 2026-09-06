// test.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <assert.h>
#include "ring_buffer.h"
#include "test_utils.h"
#include "test_ring_buffer.h"
#include "server_test.h"
#include "thread_pool_test.h"

// Define globals
int tests_run = 0;
int tests_passed = 0;
int tests_failed = 0;






int main(int argc, char **argv) {
    printf("==========================================\n");
    printf("    HTTP SERVER TEST RUNNER\n");
    printf("==========================================\n\n");

    char *suite_arg = NULL;
    char *test_filter = NULL;

    // Arg 1: Suite Name (e.g., "ring" or "server")
    if (argc > 1) suite_arg = argv[1];
    
    // Arg 2: Specific Test Filter (e.g., "resize" or "get")
    if (argc > 2) test_filter = argv[2];
    (void)test_filter;

    // --- 1. Ring Buffer Suite ---
    // Runs if no suite specified, OR if argv[1] contains "ring"
    if (!suite_arg || strstr(suite_arg, "ring")) {
        run_ring_buffer_tests();
    }

    // --- 2. General Server Suite ---
    // Runs if no suite specified, OR if argv[1] contains "server"
    if (!suite_arg || strstr(suite_arg, "server")) {
        run_server_tests();
    }

    if (!suite_arg || strstr(suite_arg, "thread_pool")) {
        run_thread_pool_suite();
    }

    // --- Summary ---
    printf("------------------------------------------\n");
    if (tests_run == 0) {
        printf(ANSI_COLOR_YELLOW "No tests matched your criteria.\n" ANSI_COLOR_RESET);
    } else {
        printf("Total Tests: %d\n", tests_run);
        if (tests_failed == 0) {
            printf(ANSI_COLOR_GREEN "VERDICT: ALL PASSED" ANSI_COLOR_RESET "\n");
            return 0;
        } else {
            printf(ANSI_COLOR_RED "VERDICT: %d FAILED" ANSI_COLOR_RESET "\n", tests_failed);
            return 1;
        }
    }
    return 0;
}
