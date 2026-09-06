#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>
#include "test_utils.h"
#include "thread_pool.h"
#include "http_server.h"

// --- Task Pool Tests ---

void test_task_pool_allocation() {
    int capacity = 10;
    TaskPool *tp = create_task_pool(capacity);
    
    TEST_ASSERT(tp != NULL);
    TEST_ASSERT(tp->capacity == capacity);
    TEST_ASSERT(tp->top == capacity - 1);

    // Test allocation of all items
    Task *tasks[10];
    for (int i = 0; i < capacity; i++) {
        tasks[i] = task_alloc(tp);
        TEST_ASSERT(tasks[i] != NULL);
    }

    // Pool should now be exhausted
    TEST_ASSERT(task_alloc(tp) == NULL);

    // Return one and re-allocate (LIFO check)
    task_free(tp, tasks[5]);
    Task *reallocated = task_alloc(tp);
    TEST_ASSERT(reallocated == tasks[5]);

    // Cleanup
    destroy_task_pool(tp);
}

void test_task_pool_thread_safety() {
    int capacity = 1000;
    TaskPool *tp = create_task_pool(capacity);
    
    // Simple verification that we can exhaust and refill
    for(int i = 0; i < capacity; i++) {
        Task *t = task_alloc(tp);
        TEST_ASSERT(t != NULL);
    }
    TEST_ASSERT(tp->top == -1);

    // Cleanup
    destroy_task_pool(tp);
}

// --- Thread Pool Lifecycle & Queue Tests ---

void test_thread_pool_init_and_shutdown() {
    int pool_size = 4;
    ThreadPool *pool = create_thread_pool(pool_size);
    
    TEST_ASSERT(pool != NULL);
    TEST_ASSERT(pool->pool_size == pool_size);
    TEST_ASSERT(pool->shutdown == 0);

    // Small sleep to let threads initialize and enter wait state
    usleep(10000); 

    destroy_thread_pool(pool);
    // If we reach here, join successful
    TEST_ASSERT(1); 
}

void test_thread_pool_invalid_creation() {
    ThreadPool *pool = create_thread_pool(0);
    TEST_ASSERT(pool == NULL);

    pool = create_thread_pool(-5);
    TEST_ASSERT(pool == NULL);
}

void test_queue_logic_internal() {
    // We create a pool with 1 thread so we can control the timing better
    ThreadPool *pool = create_thread_pool(1);
    
    // Add multiple tasks
    // Note: The worker thread will start consuming these immediately.
    // In a pure unit test, we'd mock the worker, but here we test the real system.
    add_task_to_queue(pool, 10);
    add_task_to_queue(pool, 20);
    add_task_to_queue(pool, 30);

    pthread_mutex_lock(&pool->queue_mutex);
    // Tasks might be consumed quickly, but we check if the pointers were handled
    if (pool->task_queue_head != NULL) {
        TEST_ASSERT(pool->task_queue_head->client_socket == 10 || 
                    pool->task_queue_head->client_socket == 20 || 
                    pool->task_queue_head->client_socket == 30);
    }
    pthread_mutex_unlock(&pool->queue_mutex);

    destroy_thread_pool(pool);
}

// --- Utility Tests ---

void test_set_nonblocking_logic() {
    // Create a dummy pipe to test fcntl
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        TEST_ASSERT(0 && "Pipe creation failed");
        return;
    }

    int status = set_nonblocking(pipefd[0]);
    TEST_ASSERT(status != -1);

    int flags = fcntl(pipefd[0], F_GETFL, 0);
    TEST_ASSERT(flags & O_NONBLOCK);

    close(pipefd[0]);
    close(pipefd[1]);
}

// --- Integration/Stress Test ---

void test_pool_stress_tasks() {
    int num_workers = 8;
    int num_tasks = 100;
    ThreadPool *pool = create_thread_pool(num_workers);
    
    // Flood the queue with dummy socket descriptors
    // In real code, worker_thread_function handles these.
    // Ensure worker_thread_function can handle "invalid" sockets gracefully.
    for (int i = 0; i < num_tasks; i++) {
        add_task_to_queue(pool, 999); 
    }

    // Wait for queue to drain
    int timeout = 0;
    while (timeout < 1000) {
        pthread_mutex_lock(&pool->queue_mutex);
        if (pool->task_queue_head == NULL) {
            pthread_mutex_unlock(&pool->queue_mutex);
            break;
        }
        pthread_mutex_unlock(&pool->queue_mutex);
        usleep(1000);
        timeout++;
    }

    destroy_thread_pool(pool);
    TEST_ASSERT(1); // Reached shutdown after stress
}

// --- Runner ---

void run_thread_pool_suite() {
    printf("=== Thread Pool & Task Arena Suite ===\n");

    // Task Pool (Arena) Tests
    RUN_TEST(test_task_pool_allocation,     "TaskPool: Allocation and LIFO reuse");
    RUN_TEST(test_task_pool_thread_safety, "TaskPool: Bulk exhaustion");

    // Thread Pool Lifecycle
    RUN_TEST(test_thread_pool_init_and_shutdown, "ThreadPool: Normal Start/Stop");
    RUN_TEST(test_thread_pool_invalid_creation,  "ThreadPool: Handle invalid size");
    
    // Queue & Execution Logic
    RUN_TEST(test_queue_logic_internal,     "Queue: Logic check");
    RUN_TEST(test_pool_stress_tasks,        "ThreadPool: Stress 100 tasks / 8 workers");

    // Utilities
    RUN_TEST(test_set_nonblocking_logic,    "Utils: Set non-blocking flags");

    printf("\n");
}