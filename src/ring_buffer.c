#include "ring_buffer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <stdint.h>

// --- Configuration ---
// Maximum allowed size (e.g., 16MB). Prevents memory exhaustion attacks.
#define MAX_RING_BUFFER_CAPACITY (1024 * 1024 * 16) 
#define DEFAULT_INITIAL_CAPACITY 1024

// --- Helper Functions ---

static inline size_t min_size(size_t a, size_t b) {
    return (a < b) ? a : b;
}

// Resizes the buffer.
// Strategy: Linearize the data (unwrap it) into the new buffer.
static int ring_buffer_resize(RingBuffer *rb, size_t new_capacity) {
    if (!rb) return -1;
    
    // Safety check: Don't shrink below current data size
    if (new_capacity < rb->size) return -1;

    // Safety check: Hard limit on memory usage
    if (new_capacity > MAX_RING_BUFFER_CAPACITY) {
        // fprintf(stderr, "Error: RingBuffer max capacity reached.\n");
        errno = ENOMEM;
        return -1;
    }

    char *new_buffer = malloc(new_capacity);
    if (!new_buffer) {
        return -1;
    }

    // Copy data to new buffer, linearizing it (Tail -> End, Start -> Head)
    if (rb->size > 0) {
        size_t to_end = rb->capacity - rb->tail;
        if (rb->head > rb->tail) {
            // Data is contiguous
            memcpy(new_buffer, rb->buffer + rb->tail, rb->size);
        } else {
            // Data wraps around
            memcpy(new_buffer, rb->buffer + rb->tail, to_end);
            memcpy(new_buffer + to_end, rb->buffer, rb->head);
        }
    }

    free(rb->buffer);
    rb->buffer = new_buffer;
    rb->capacity = new_capacity;
    rb->tail = 0;
    rb->head = rb->size; // Head points exactly after the last byte
    
    return 0;
}

// Internal helper to advance tail without reading data (used by readline)
static void ring_buffer_skip(RingBuffer *rb, size_t len) {
    if (!rb || len == 0 || rb->size == 0) return;
    
    if (len > rb->size) len = rb->size;

    rb->tail += len;
    // Handle wrap-around
    if (rb->tail >= rb->capacity) {
        rb->tail -= rb->capacity;
    }
    rb->size -= len;
}

// --- Lifecycle Functions ---

RingBuffer *ring_buffer_create(size_t initial_capacity) {
    if (initial_capacity == 0) initial_capacity = DEFAULT_INITIAL_CAPACITY;

    RingBuffer *rb = malloc(sizeof(RingBuffer));
    if (!rb) return NULL;

    rb->buffer = malloc(initial_capacity);
    if (!rb->buffer) {
        free(rb);
        return NULL;
    }

    rb->capacity = initial_capacity;
    ring_buffer_reset(rb);
    return rb;
}

void ring_buffer_free(RingBuffer *rb) {
    if (rb) {
        free(rb->buffer);
        free(rb);
    }
}

void ring_buffer_reset(RingBuffer *rb) {
    if (rb) {
        rb->head = 0;
        rb->tail = 0;
        rb->size = 0;
    }
}

// --- Core Operations ---

size_t ring_buffer_write(RingBuffer *rb, const char *data, size_t data_len) {
    if (!rb || !data || data_len == 0) return 0;

    size_t available = rb->capacity - rb->size;

    // 1. Resize if necessary
    if (data_len > available) {
        size_t new_cap = rb->capacity;
        size_t required = rb->size + data_len;

        // Exponential growth strategy (Doubling)
        while (new_cap < required) {
            new_cap *= 2;
            // Overflow check for size_t wrapping
            if (new_cap < rb->capacity) {
                new_cap = MAX_RING_BUFFER_CAPACITY + 1; // Force failure in next check
                break;
            }
        }

        // Try to resize. If it fails (OOM or Max Limit), return 0.
        // We do NOT write partial data. Atomic failure is safer for HTTP.
        if (ring_buffer_resize(rb, new_cap) != 0) {
            return 0; 
        }
    }

    // 2. Write data (Guaranteed to fit now)
    size_t to_end = rb->capacity - rb->head;

    if (data_len <= to_end) {
        // Continuous write
        memcpy(rb->buffer + rb->head, data, data_len);
        rb->head += data_len;
        if (rb->head == rb->capacity) rb->head = 0;
    } else {
        // Wrap-around write
        memcpy(rb->buffer + rb->head, data, to_end);
        memcpy(rb->buffer, data + to_end, data_len - to_end);
        rb->head = data_len - to_end;
    }

    rb->size += data_len;
    return data_len;
}

size_t ring_buffer_read(RingBuffer *rb, char *dest, size_t dest_len) {
    if (!rb || !dest || dest_len == 0 || rb->size == 0) return 0;

    // Cap read length to available data
    size_t bytes_to_read = min_size(dest_len, rb->size);
    size_t to_end = rb->capacity - rb->tail;

    if (bytes_to_read <= to_end) {
        // Continuous read
        memcpy(dest, rb->buffer + rb->tail, bytes_to_read);
        rb->tail += bytes_to_read;
        if (rb->tail == rb->capacity) rb->tail = 0;
    } else {
        // Wrap-around read
        memcpy(dest, rb->buffer + rb->tail, to_end);
        memcpy(dest + to_end, rb->buffer, bytes_to_read - to_end);
        rb->tail = bytes_to_read - to_end;
    }

    rb->size -= bytes_to_read;
    return bytes_to_read;
}

size_t ring_buffer_peek(const RingBuffer *rb, char *dest, size_t dest_len) {
    if (!rb || !dest || dest_len == 0 || rb->size == 0) return 0;

    size_t bytes_to_peek = min_size(dest_len, rb->size);
    size_t to_end = rb->capacity - rb->tail;

    if (bytes_to_peek <= to_end) {
        memcpy(dest, rb->buffer + rb->tail, bytes_to_peek);
    } else {
        memcpy(dest, rb->buffer + rb->tail, to_end);
        memcpy(dest + to_end, rb->buffer, bytes_to_peek - to_end);
    }

    return bytes_to_peek;
}

// --- Specialized HTTP Operations ---

char *ring_buffer_readline(RingBuffer *rb, char *line_buffer, size_t line_buffer_size) {
    if (!rb || !line_buffer || line_buffer_size == 0 || rb->size == 0) {
        if (line_buffer && line_buffer_size > 0) line_buffer[0] = '\0';
        return NULL;
    }

    // We scan for '\n'.
    // Optimization: Use memchr instead of looping byte-by-byte.
    // Because the buffer wraps, we might need two scans.

    size_t to_end = rb->capacity - rb->tail;
    size_t search_len_1 = min_size(rb->size, to_end);
    
    // 1. Scan from Tail to End of Buffer
    void *found_ptr = memchr(rb->buffer + rb->tail, '\n', search_len_1);
    size_t newline_offset = 0;
    int found = 0;

    if (found_ptr) {
        newline_offset = (char*)found_ptr - (rb->buffer + rb->tail);
        found = 1;
    } 
    // 2. If not found and data wraps, scan from Start to Head
    else if (rb->size > to_end) {
        size_t search_len_2 = rb->size - to_end;
        found_ptr = memchr(rb->buffer, '\n', search_len_2);
        if (found_ptr) {
            // Offset is part1 length + distance into part2
            newline_offset = to_end + ((char*)found_ptr - rb->buffer);
            found = 1;
        }
    }

    if (!found) {
        return NULL; // No complete line found
    }

    // Total bytes to remove from ring buffer (chars + \n)
    size_t total_bytes_to_consume = newline_offset + 1;

    // Calculate effective string length (excluding \n and potential \r)
    size_t content_len = newline_offset;
    
    // Check for \r (Carriage Return) before \n
    if (newline_offset > 0) {
        // Need to peek the character before the newline.
        // Since newline_offset is relative to tail, (tail + offset - 1) handles wrap logic.
        size_t prev_idx = rb->tail + newline_offset - 1;
        if (prev_idx >= rb->capacity) prev_idx -= rb->capacity; // Wrap correction
        
        if (rb->buffer[prev_idx] == '\r') {
            content_len--;
        }
    } else {
        // Special case: newline is at offset 0. 
        // Logic dictates we check the LAST byte of the PREVIOUS write? 
        // No, standard readline assumes the \r is currently in the buffer.
        // If the buffer starts with \n, content_len is 0.
    }

    // How much to copy to user buffer? (Protect against overflow)
    size_t bytes_to_copy = content_len;
    if (bytes_to_copy >= line_buffer_size) {
        bytes_to_copy = line_buffer_size - 1;
    }

    // Reuse PEEK logic to copy the specific line content
    // We cannot use ring_buffer_read yet because we want to discard the *full* line (incl \r\n),
    // even if we only copy a truncated portion to line_buffer.
    
    size_t part1_len = min_size(bytes_to_copy, rb->capacity - rb->tail);
    memcpy(line_buffer, rb->buffer + rb->tail, part1_len);
    
    if (bytes_to_copy > part1_len) {
        memcpy(line_buffer + part1_len, rb->buffer, bytes_to_copy - part1_len);
    }
    
    line_buffer[bytes_to_copy] = '\0';

    // Discard the processed line from the ring buffer
    ring_buffer_skip(rb, total_bytes_to_consume);

    return line_buffer;
}

// --- Getters ---

size_t ring_buffer_get_size(const RingBuffer *rb) {
    return rb ? rb->size : 0;
}

size_t ring_buffer_get_capacity(const RingBuffer *rb) {
    return rb ? rb->capacity : 0;
}

int ring_buffer_is_empty(const RingBuffer *rb) {
    return (!rb || rb->size == 0);
}

int ring_buffer_is_full(const RingBuffer *rb) {
    return (rb && rb->size == rb->capacity);
}