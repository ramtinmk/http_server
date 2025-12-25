#include "thread_pool.h"
#include "http_server.h"


TaskPool *create_task_pool(int capacity)
{
    TaskPool *tp = malloc(sizeof(TaskPool));

    // Allocate ONE big block for all tasks (Arena-style locality)
    tp->pool_storage = malloc(sizeof(Task) * capacity);

    // Allocate the stack that tracks which ones are free
    tp->free_stack = malloc(sizeof(Task *) * capacity);
    tp->capacity = capacity;
    tp->top = -1;
    pthread_mutex_init(&tp->lock, NULL);

    // Fill stack: Initially, ALL tasks are free
    for (int i = 0; i < capacity; i++)
    {
        tp->top++;
        tp->free_stack[tp->top] = &tp->pool_storage[i];
    }
    return tp;
}

// --- Buffer Pool Implementation ---
BufferPool *create_buffer_pool(int capacity)
{
    BufferPool *bp = malloc(sizeof(BufferPool));
    bp->pool_storage = malloc(sizeof(RingBuffer *) * capacity);
    bp->capacity = capacity;
    bp->top = -1;
    pthread_mutex_init(&bp->lock, NULL);

    for (int i = 0; i < capacity; i++)
    {
        // Pre-allocate the actual RingBuffers here!
        // We assume your ring_buffer_create uses malloc internally.
        // We do this ONCE at startup.
        bp->top++;
        bp->pool_storage[bp->top] = ring_buffer_create(INITIAL_RING_BUFFER_CAPACITY);
    }
    return bp;
}


ThreadPool *create_thread_pool(int pool_size)
{
    if (pool_size <= 0)
    {
        fprintf(stderr, "Error: Pool size must be greater than 0\n");
        return NULL;
    }

    ThreadPool *pool = malloc(sizeof(ThreadPool));
    if (!pool)
    {
        perror("Failed to allocate thread pool");
        return NULL;
    }

    pool->pool_size = pool_size;
    pool->threads = malloc(sizeof(pthread_t) * pool_size);
    if (!pool->threads)
    {
        perror("Failed to allocate thread array");
        free(pool);
        return NULL;
    }
    pool->task_pool = create_task_pool(pool_size * 4);
    pool->buffer_pool = create_buffer_pool(pool_size);

    if (!pool->task_pool || !pool->buffer_pool)
    {
        // Handle error...
        return NULL;
    }

    pool->task_queue_head = NULL;
    pool->task_queue_tail = NULL;
    if (pthread_mutex_init(&pool->queue_mutex, NULL) != 0)
    {
        perror("Mutex initialization failed");
        free(pool->threads);
        free(pool);
        return NULL;
    }
    if (pthread_cond_init(&pool->queue_cond, NULL) != 0)
    {
        perror("Condition variable initialization failed");
        pthread_mutex_destroy(&pool->queue_mutex);
        free(pool->threads);
        free(pool);
        return NULL;
    }
    pool->shutdown = 0;

    for (int i = 0; i < pool_size; i++)
    {
        if (pthread_create(&pool->threads[i], NULL, worker_thread_function, pool) != 0)
        {
            perror("Failed to create worker thread");
            // In a real-world scenario, you'd want to handle thread creation failure more gracefully,
            // potentially destroying already created threads and pool resources.
            destroy_thread_pool(pool); // Attempt to cleanup partially created pool
            return NULL;
        }
    }

    return pool;
}


void destroy_thread_pool(ThreadPool *pool)
{
    if (!pool)
        return;

    pthread_mutex_lock(&pool->queue_mutex);
    pool->shutdown = 1;                        // Set shutdown flag
    pthread_cond_broadcast(&pool->queue_cond); // Wake up all worker threads
    pthread_mutex_unlock(&pool->queue_mutex);

    for (int i = 0; i < pool->pool_size; i++)
    {
        if (pthread_join(pool->threads[i], NULL) != 0)
        {
            perror("Failed to join worker thread");
        }
    }

    pthread_mutex_destroy(&pool->queue_mutex);
    pthread_cond_destroy(&pool->queue_cond);

    // Free task queue (important to free any remaining tasks if queue wasn't fully processed)
    Task *current_task = pool->task_queue_head;
    while (current_task != NULL)
    {
        Task *next_task = current_task->next;
        free(current_task);
        current_task = next_task;
    }

    free(pool->threads);
    free(pool);
}



RingBuffer *buffer_acquire(BufferPool *bp)
{
    pthread_mutex_lock(&bp->lock);
    if (bp->top == -1)
    {
        pthread_mutex_unlock(&bp->lock);
        // Fallback: If pool is empty, create a temporary one (slower path)
        return ring_buffer_create(INITIAL_RING_BUFFER_CAPACITY);
    }
    RingBuffer *rb = bp->pool_storage[bp->top];
    bp->top--;
    pthread_mutex_unlock(&bp->lock);

    // If you have a specific ring_buffer_reset() function, use that instead.

    ring_buffer_reset(rb);

    return rb;
}

void buffer_release(BufferPool *bp, RingBuffer *rb)
{
    pthread_mutex_lock(&bp->lock);
    if (bp->top < bp->capacity - 1)
    {
        bp->top++;
        bp->pool_storage[bp->top] = rb;
    }
    else
    {
        // Pool is full? This might be a temporary one we created. Free it.
        ring_buffer_free(rb);
    }
    pthread_mutex_unlock(&bp->lock);
}





void add_task_to_queue(ThreadPool *pool, int client_socket)
{
    if (!pool)
        return;

    // Task *new_task = malloc(sizeof(Task));
    Task *new_task = task_alloc(pool->task_pool);
    if (!new_task)
    {
        perror("Failed to allocate task");
        close(client_socket); // Important: close socket if task allocation fails
        return;
    }
    new_task->client_socket = client_socket;
    new_task->next = NULL;

    pthread_mutex_lock(&pool->queue_mutex);
    if (pool->task_queue_tail == NULL)
    {
        pool->task_queue_head = new_task;
        pool->task_queue_tail = new_task;
    }
    else
    {
        pool->task_queue_tail->next = new_task;
        pool->task_queue_tail = new_task;
    }

    pthread_cond_signal(&pool->queue_cond); // Signal a worker thread that a task is available
    pthread_mutex_unlock(&pool->queue_mutex);
}

Task *get_task_from_queue(ThreadPool *pool)
{
    if (!pool)
        return NULL;

    pthread_mutex_lock(&pool->queue_mutex);

    // Wait while queue is empty and server is not shutting down
    while (pool->task_queue_head == NULL && !pool->shutdown)
    {
        pthread_cond_wait(&pool->queue_cond, &pool->queue_mutex);
    }

    if (pool->shutdown && pool->task_queue_head == NULL)
    {
        pthread_mutex_unlock(&pool->queue_mutex);
        return NULL; // Signal for thread to exit
    }

    Task *task = pool->task_queue_head;
    if (task != NULL)
    {
        pool->task_queue_head = task->next;
        if (pool->task_queue_head == NULL)
        {
            pool->task_queue_tail = NULL; // Queue became empty
        }
    }

    pthread_mutex_unlock(&pool->queue_mutex);
    return task;
}


int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}



void task_free(TaskPool *tp, Task *t)
{
    pthread_mutex_lock(&tp->lock);
    if (tp->top < tp->capacity - 1)
    {
        tp->top++;
        tp->free_stack[tp->top] = t; // Push back
    }
    pthread_mutex_unlock(&tp->lock);
}


Task *task_alloc(TaskPool *tp)
{
    pthread_mutex_lock(&tp->lock);
    if (tp->top == -1)
    {
        pthread_mutex_unlock(&tp->lock);
        return NULL; // Pool empty! (In production, handle this gracefully)
    }
    Task *t = tp->free_stack[tp->top]; // Pop
    tp->top--;
    pthread_mutex_unlock(&tp->lock);
    return t;
}
