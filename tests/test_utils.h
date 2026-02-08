#ifndef TEST_UTILS_H
#define TEST_UTILS_H

#include <stdio.h>

extern int tests_run;
extern int tests_passed;
extern int tests_failed;

#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_YELLOW  "\x1b[33m"
#define ANSI_COLOR_RESET   "\x1b[0m"

#define TEST_ASSERT(cond) do { \
    if (!(cond)) { \
        printf(ANSI_COLOR_RED "    [FAIL] %s:%d: Assertion '%s' failed" ANSI_COLOR_RESET "\n", __func__, __LINE__, #cond); \
        tests_failed++; \
        return; \
    } \
} while(0)

// Modified to accept a 'desc' string
#define RUN_TEST(func, desc) do { \
    printf(ANSI_COLOR_YELLOW "[TEST] %-30s" ANSI_COLOR_RESET " : %s\n", #func, desc); \
    int failed_before = tests_failed; \
    func(); \
    if (tests_failed == failed_before) { \
        printf(ANSI_COLOR_GREEN "       -> PASS" ANSI_COLOR_RESET "\n"); \
        tests_passed++; \
    } else { \
        printf(ANSI_COLOR_RED "       -> FAIL" ANSI_COLOR_RESET "\n"); \
    } \
    tests_run++; \
    printf("\n"); \
} while(0)

#endif // TEST_UTILS_H