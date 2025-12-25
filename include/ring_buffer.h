#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <stddef.h> // For size_t
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>

// --- Configuration ---
#define INITIAL_RING_BUFFER_CAPACITY 8092

typedef struct RingBuffer RingBuffer;

struct RingBuffer{
    char *buffer;
    size_t capacity;
    size_t head; // Write position
    size_t tail; // Read position
    size_t size; // Current data size in buffer
};

RingBuffer *ring_buffer_create(size_t capacity);
void ring_buffer_free(RingBuffer *rb);
size_t ring_buffer_write(RingBuffer *rb, const char *data, size_t data_len);
size_t ring_buffer_read(RingBuffer *rb, char *dest, size_t dest_len);
size_t ring_buffer_peek(const RingBuffer *rb, char *dest, size_t dest_len); // Non-consuming read
void ring_buffer_reset(RingBuffer *rb);
size_t ring_buffer_get_size(const RingBuffer *rb);
size_t ring_buffer_get_capacity(const RingBuffer *rb);
int ring_buffer_is_empty(const RingBuffer *rb);
int ring_buffer_is_full(const RingBuffer *rb);
char *ring_buffer_readline(RingBuffer *rb, char *line_buffer, size_t line_buffer_size);



#endif