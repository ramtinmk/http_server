#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <pthread.h>
#include "ring_buffer.h"


#define THREAD_POOL_SIZE 16

// Forward declaration
typedef struct Task Task;
typedef struct TaskPool TaskPool;
typedef struct BufferPool BufferPool;
typedef struct ThreadPool ThreadPool;

/* 2. Struct Definitions */

struct Task {
    int client_socket;
    Task *next; 
};

struct TaskPool {
    Task *pool_storage;     
    Task **free_stack;      
    int top;                
    int capacity;
    pthread_mutex_t lock;
};

struct BufferPool {
    // START CHECK: Ensure "RingBuffer" is actually defined in "ring_buffer.h"
    // If RingBuffer is a typedef, this is fine. 
    // If RingBuffer is a struct tag, use "struct RingBuffer **pool_storage;"
    RingBuffer **pool_storage; 
    // END CHECK
    
    int top;
    int capacity;
    pthread_mutex_t lock;
};

struct ThreadPool {
    int pool_size;
    pthread_t *threads;
    
    Task *task_queue_head;
    Task *task_queue_tail;
    
    pthread_mutex_t queue_mutex;
    pthread_cond_t queue_cond;
    int shutdown;

    // These now definitely work because of the forward declarations above
    TaskPool *task_pool;
    BufferPool *buffer_pool;
};


ThreadPool *create_thread_pool(int pool_size);
void destroy_thread_pool(ThreadPool *pool);
void add_task_to_queue(ThreadPool *pool, int client_socket);
Task *get_task_from_queue(ThreadPool *pool); // Or Task* get_task_from_queue(...) depending on task structure return
void *worker_thread_function(void *arg);
TaskPool *create_task_pool(int capacity);
void buffer_release(BufferPool *bp, RingBuffer *rb);
RingBuffer *buffer_acquire(BufferPool *bp);
void add_task_to_queue(ThreadPool *pool, int client_socket);
Task *get_task_from_queue(ThreadPool *pool);
Task *task_alloc(TaskPool *tp);
void task_free(TaskPool *tp, Task *t);






#endif