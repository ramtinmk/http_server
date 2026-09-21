#include "ring_buffer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <stdint.h>
#include <assert.h>

// --- Configuration ---
// Maximum allowed size (e.g., 16MB). Prevents memory exhaustion attacks.
#define MAX_RING_BUFFER_CAPACITY ((size_t)16 * 1024 * 1024)
#define DEFAULT_INITIAL_CAPACITY 1024

// --- Helper Functions ---

static inline size_t min_size(size_t a, size_t b) {
    return (a < b) ? a : b;
}

// Invariant checker used by assertions after every mutation in debug builds.
int ring_buffer_validate(const RingBuffer *rb) {
    if (!rb || !rb->buffer || rb->capacity == 0) { return 0; }
    if (rb->size > rb->capacity) { return 0; }
    if (rb->head >= rb->capacity || rb->tail >= rb->capacity) { return 0; }
    // head is always tail advanced by size, modulo capacity.
    if ((rb->tail + rb->size) % rb->capacity != rb->head) { return 0; }
    return 1;
}

// Resizes the buffer.
// Strategy: Linearize the data (unwrap it) into the new buffer.
static int ring_buffer_resize(RingBuffer *rb, size_t new_capacity) {
    if (!rb) { return -1;
}
    
    // Safety check: Don't shrink below current data size
    if (new_capacity < rb->size) { return -1;
}

    // Safety check: Hard limit on memory usage
    if (new_capacity > MAX_RING_BUFFER_CAPACITY) {
        errno = ENOMEM;
        return -1;
    }

    char *new_buffer = malloc(new_capacity);
    if (!new_buffer) {
        return -1;
    }

    // Copy data to new buffer, linearizing it (Tail -> End, Start -> Head)
    if (rb->size > 0 && rb->buffer) {
        size_t to_end = rb->capacity - rb->tail;
        size_t part1 = min_size(rb->size, to_end);
        memcpy(new_buffer, rb->buffer + rb->tail, part1);
        if (rb->size > part1) {
            memcpy(new_buffer + part1, rb->buffer, rb->size - part1);
        }
    }

    free(rb->buffer);
    rb->buffer = new_buffer;
    rb->capacity = new_capacity;
    rb->tail = 0;
    rb->head = (rb->size == new_capacity) ? 0 : rb->size;

    assert(ring_buffer_validate(rb));
    return 0;
}

// Internal helper to advance tail without reading data (used by readline)
static void ring_buffer_skip(RingBuffer *rb, size_t len) {
    if (!rb || len == 0 || rb->size == 0) { return;
}
    
    if (len > rb->size) { len = rb->size;
}

    rb->tail += len;
    // A single subtraction is sufficient: len <= rb->size <= rb->capacity,
    // so tail can never overshoot capacity by more than one lap.
    if (rb->tail >= rb->capacity) {
        rb->tail -= rb->capacity;
    }
    rb->size -= len;

    assert(ring_buffer_validate(rb));
}

// --- Lifecycle Functions ---

RingBuffer *ring_buffer_create(size_t initial_capacity) {
    if (initial_capacity == 0) { initial_capacity = DEFAULT_INITIAL_CAPACITY;
}
    if (initial_capacity > MAX_RING_BUFFER_CAPACITY) {
        initial_capacity = MAX_RING_BUFFER_CAPACITY;
    }

    RingBuffer *rb = malloc(sizeof(RingBuffer));
    if (!rb) { return NULL;
}

    rb->buffer = malloc(initial_capacity);
    if (!rb->buffer) {
        free(rb);
        return NULL;
    }

    rb->capacity = initial_capacity;
    ring_buffer_reset(rb);
    assert(ring_buffer_validate(rb));
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
        assert(ring_buffer_validate(rb));
    }
}

// --- Core Operations ---

size_t ring_buffer_write(RingBuffer *rb, const char *data, size_t data_len) {
    if (!rb || !data || data_len == 0) { return 0;
}

    // Overflow-safe admission against the hard ceiling.  Checking this before
    // computing rb->size + data_len avoids any size_t wraparound.
    if (data_len > MAX_RING_BUFFER_CAPACITY - rb->size) {
        errno = ENOMEM;
        return 0;
    }

    size_t available = rb->capacity - rb->size;

    // 1. Resize if necessary
    if (data_len > available) {
        size_t new_cap = rb->capacity ? rb->capacity : DEFAULT_INITIAL_CAPACITY;
        size_t required = rb->size + data_len;

        // Exponential growth strategy (Doubling)
        while (new_cap < required) {
            if (new_cap > MAX_RING_BUFFER_CAPACITY / 2) {
                new_cap = MAX_RING_BUFFER_CAPACITY;
                break;
            }
            new_cap *= 2;
        }

        if (new_cap < required) {
            new_cap = required;
        }

        // Try to resize. If it fails (OOM or Max Limit), return 0.
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
        if (rb->head == rb->capacity) { rb->head = 0;
}
    } else {
        // Wrap-around write
        memcpy(rb->buffer + rb->head, data, to_end);
        memcpy(rb->buffer, data + to_end, data_len - to_end);
        rb->head = data_len - to_end;
    }

    rb->size += data_len;

    assert(ring_buffer_validate(rb));
    return data_len;
}

size_t ring_buffer_read(RingBuffer *rb, char *dest, size_t dest_len) {
    if (!rb || !dest || dest_len == 0 || rb->size == 0) { return 0;
}

    // Cap read length to available data
    size_t bytes_to_read = min_size(dest_len, rb->size);
    size_t to_end = rb->capacity - rb->tail;

    if (bytes_to_read <= to_end) {
        // Continuous read
        memcpy(dest, rb->buffer + rb->tail, bytes_to_read);
        rb->tail += bytes_to_read;
        if (rb->tail == rb->capacity) { rb->tail = 0;
}
    } else {
        // Wrap-around read
        memcpy(dest, rb->buffer + rb->tail, to_end);
        memcpy(dest + to_end, rb->buffer, bytes_to_read - to_end);
        rb->tail = bytes_to_read - to_end;
    }

    rb->size -= bytes_to_read;

    assert(ring_buffer_validate(rb));
    return bytes_to_read;
}

size_t ring_buffer_peek(const RingBuffer *rb, char *dest, size_t dest_len) {
    if (!rb || !dest || dest_len == 0 || rb->size == 0) { return 0;
}

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

// --- Specialized Line Operations ---

RingLineResult ring_buffer_readline(RingBuffer *rb, char *line_buffer,
                                    size_t line_buffer_size, size_t *line_length) {
    if (line_length) { *line_length = 0;
}

    if (!rb || !line_buffer || line_buffer_size == 0) {
        return RING_LINE_NONE;
    }

    line_buffer[0] = '\0';

    if (rb->size == 0) {
        return RING_LINE_NONE;
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
        return RING_LINE_NONE; // No complete line found
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
        if (prev_idx >= rb->capacity) { prev_idx -= rb->capacity; // Wrap correction
}
        
        if (rb->buffer[prev_idx] == '\r') {
            content_len--;
        }
    } else {
        // Special case: newline is at offset 0. 
        // Logic dictates we check the LAST byte of the PREVIOUS write? 
        // No, standard readline assumes the \r is currently in the buffer.
        // If the buffer starts with \n, content_len is 0.
    }

    // Report the true content length even when it will be truncated, so the
    // caller can enforce a maximum line length.
    if (line_length) { *line_length = content_len;
}

    // Determine whether the line fits (reserving one byte for the NUL).
    RingLineResult result;
    size_t bytes_to_copy = content_len;
    if (bytes_to_copy >= line_buffer_size) {
        bytes_to_copy = line_buffer_size - 1;
        result = RING_LINE_TOO_LONG;
    } else {
        result = RING_LINE_OK;
    }

    // Copy the specific line content.  We cannot use ring_buffer_read here
    // because the *full* line (including \r\n) must be discarded even when
    // only a truncated prefix is copied to line_buffer.

    size_t part1_len = min_size(bytes_to_copy, rb->capacity - rb->tail);
    memcpy(line_buffer, rb->buffer + rb->tail, part1_len);
    
    if (bytes_to_copy > part1_len) {
        memcpy(line_buffer + part1_len, rb->buffer, bytes_to_copy - part1_len);
    }
    
    line_buffer[bytes_to_copy] = '\0';

    // Discard the processed line from the ring buffer
    ring_buffer_skip(rb, total_bytes_to_consume);

    return result;
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
