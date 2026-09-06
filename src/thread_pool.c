#include "thread_pool.h"
#include "http_server.h"

TaskPool *create_task_pool(int capacity)
{
    if (capacity <= 0) return NULL;

    TaskPool *tp = malloc(sizeof(TaskPool));
    if (!tp) return NULL;

    tp->pool_storage = malloc(sizeof(Task) * capacity);
    tp->free_stack = malloc(sizeof(Task *) * capacity);
    if (!tp->pool_storage || !tp->free_stack)
    {
        free(tp->pool_storage);
        free(tp->free_stack);
        free(tp);
        return NULL;
    }

    tp->capacity = capacity;
    tp->top = -1;
    if (pthread_mutex_init(&tp->lock, NULL) != 0)
    {
        free(tp->pool_storage);
        free(tp->free_stack);
        free(tp);
        return NULL;
    }

    for (int i = 0; i < capacity; i++)
    {
        tp->top++;
        tp->free_stack[tp->top] = &tp->pool_storage[i];
    }
    return tp;
}

void destroy_task_pool(TaskPool *tp)
{
    if (!tp) return;
    pthread_mutex_destroy(&tp->lock);
    free(tp->pool_storage);
    free(tp->free_stack);
    free(tp);
}

// --- Buffer Pool Implementation ---
BufferPool *create_buffer_pool(int capacity)
{
    if (capacity <= 0) return NULL;

    BufferPool *bp = malloc(sizeof(BufferPool));
    if (!bp) return NULL;

    bp->pool_storage = malloc(sizeof(RingBuffer *) * capacity);
    if (!bp->pool_storage)
    {
        free(bp);
        return NULL;
    }

    bp->capacity = capacity;
    bp->top = -1;
    if (pthread_mutex_init(&bp->lock, NULL) != 0)
    {
        free(bp->pool_storage);
        free(bp);
        return NULL;
    }

    for (int i = 0; i < capacity; i++)
    {
        RingBuffer *rb = ring_buffer_create(INITIAL_RING_BUFFER_CAPACITY);
        if (!rb)
        {
            destroy_buffer_pool(bp);
            return NULL;
        }
        bp->top++;
        bp->pool_storage[bp->top] = rb;
    }
    return bp;
}

void destroy_buffer_pool(BufferPool *bp)
{
    if (!bp) return;
    pthread_mutex_lock(&bp->lock);
    for (int i = 0; i <= bp->top; i++)
    {
        ring_buffer_free(bp->pool_storage[i]);
    }
    bp->top = -1;
    pthread_mutex_unlock(&bp->lock);

    pthread_mutex_destroy(&bp->lock);
    free(bp->pool_storage);
    free(bp);
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

    pool->task_pool = create_task_pool(pool_size * 16);
    if (!pool->task_pool)
    {
        free(pool->threads);
        free(pool);
        return NULL;
    }

    pool->buffer_pool = create_buffer_pool(pool_size);
    if (!pool->buffer_pool)
    {
        destroy_task_pool(pool->task_pool);
        free(pool->threads);
        free(pool);
        return NULL;
    }

    pool->task_queue_head = NULL;
    pool->task_queue_tail = NULL;
    pool->shutdown = 0;

    if (pthread_mutex_init(&pool->queue_mutex, NULL) != 0)
    {
        perror("Mutex initialization failed");
        destroy_buffer_pool(pool->buffer_pool);
        destroy_task_pool(pool->task_pool);
        free(pool->threads);
        free(pool);
        return NULL;
    }

    if (pthread_cond_init(&pool->queue_cond, NULL) != 0)
    {
        perror("Condition variable initialization failed");
        pthread_mutex_destroy(&pool->queue_mutex);
        destroy_buffer_pool(pool->buffer_pool);
        destroy_task_pool(pool->task_pool);
        free(pool->threads);
        free(pool);
        return NULL;
    }

    int threads_created = 0;
    for (int i = 0; i < pool_size; i++)
    {
        if (pthread_create(&pool->threads[i], NULL, worker_thread_function, pool) != 0)
        {
            perror("Failed to create worker thread");
            pool->pool_size = threads_created;
            destroy_thread_pool(pool);
            return NULL;
        }
        threads_created++;
    }

    return pool;
}

void destroy_thread_pool(ThreadPool *pool)
{
    if (!pool)
        return;

    pthread_mutex_lock(&pool->queue_mutex);
    pool->shutdown = 1;
    pthread_cond_broadcast(&pool->queue_cond);
    pthread_mutex_unlock(&pool->queue_mutex);

    for (int i = 0; i < pool->pool_size; i++)
    {
        pthread_join(pool->threads[i], NULL);
    }

    pthread_mutex_destroy(&pool->queue_mutex);
    pthread_cond_destroy(&pool->queue_cond);

    // Close any unhandled sockets in the queue
    Task *current_task = pool->task_queue_head;
    while (current_task != NULL)
    {
        if (current_task->client_socket >= 0)
        {
            close(current_task->client_socket);
        }
        current_task = current_task->next;
    }
    pool->task_queue_head = NULL;
    pool->task_queue_tail = NULL;

    destroy_task_pool(pool->task_pool);
    destroy_buffer_pool(pool->buffer_pool);

    free(pool->threads);
    free(pool);
}

RingBuffer *buffer_acquire(BufferPool *bp)
{
    if (!bp) return NULL;

    pthread_mutex_lock(&bp->lock);
    if (bp->top == -1)
    {
        pthread_mutex_unlock(&bp->lock);
        return ring_buffer_create(INITIAL_RING_BUFFER_CAPACITY);
    }
    RingBuffer *rb = bp->pool_storage[bp->top];
    bp->top--;
    pthread_mutex_unlock(&bp->lock);

    ring_buffer_reset(rb);
    return rb;
}

void buffer_release(BufferPool *bp, RingBuffer *rb)
{
    if (!rb) return;
    if (!bp)
    {
        ring_buffer_free(rb);
        return;
    }

    // Shrink excessively large ring buffers to avoid memory retention
    if (rb->capacity > INITIAL_RING_BUFFER_CAPACITY * 8)
    {
        char *new_buf = malloc(INITIAL_RING_BUFFER_CAPACITY);
        if (new_buf)
        {
            free(rb->buffer);
            rb->buffer = new_buf;
            rb->capacity = INITIAL_RING_BUFFER_CAPACITY;
        }
    }
    ring_buffer_reset(rb);

    pthread_mutex_lock(&bp->lock);
    if (bp->top < bp->capacity - 1)
    {
        bp->top++;
        bp->pool_storage[bp->top] = rb;
        pthread_mutex_unlock(&bp->lock);
    }
    else
    {
        pthread_mutex_unlock(&bp->lock);
        ring_buffer_free(rb);
    }
}

void add_task_to_queue(ThreadPool *pool, int client_socket)
{
    if (!pool)
    {
        if (client_socket >= 0) close(client_socket);
        return;
    }

    Task *new_task = task_alloc(pool->task_pool);
    if (!new_task)
    {
        fprintf(stderr, "Error: Task pool exhausted. Dropping connection on socket %d\n", client_socket);
        if (client_socket >= 0) close(client_socket); 
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

    pthread_cond_signal(&pool->queue_cond);
    pthread_mutex_unlock(&pool->queue_mutex);
}

Task *get_task_from_queue(ThreadPool *pool)
{
    if (!pool)
        return NULL;

    pthread_mutex_lock(&pool->queue_mutex);

    while (pool->task_queue_head == NULL && !pool->shutdown)
    {
        pthread_cond_wait(&pool->queue_cond, &pool->queue_mutex);
    }

    if (pool->shutdown && pool->task_queue_head == NULL)
    {
        pthread_mutex_unlock(&pool->queue_mutex);
        return NULL;
    }

    Task *task = pool->task_queue_head;
    if (task != NULL)
    {
        pool->task_queue_head = task->next;
        if (pool->task_queue_head == NULL)
        {
            pool->task_queue_tail = NULL;
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
    if (!tp || !t) return;

    pthread_mutex_lock(&tp->lock);
    if (tp->top < tp->capacity - 1)
    {
        tp->top++;
        tp->free_stack[tp->top] = t;
    }
    pthread_mutex_unlock(&tp->lock);
}

Task *task_alloc(TaskPool *tp)
{
    if (!tp) return NULL;

    pthread_mutex_lock(&tp->lock);
    if (tp->top == -1)
    {
        pthread_mutex_unlock(&tp->lock);
        return NULL;
    }
    Task *t = tp->free_stack[tp->top];
    tp->top--;
    pthread_mutex_unlock(&tp->lock);
    return t;
}
