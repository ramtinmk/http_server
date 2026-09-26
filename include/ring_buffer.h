#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <stddef.h> // For size_t

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

/*
 * Result of ring_buffer_readline().
 *
 * The distinction between OK and TOO_LONG matters for an HTTP parser: a line
 * that does not fit is still consumed in full, so without this status the
 * caller cannot tell a genuinely short line from a truncated long one.
 */
typedef enum {
    RING_LINE_NONE = 0,  // No complete line (no '\n') yet; buffer unchanged.
    RING_LINE_OK,        // Complete line copied and consumed.
    RING_LINE_TOO_LONG   // Complete line consumed, but truncated into the
                         // destination buffer (did not fit).
} RingLineResult;

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

/*
 * Reads one '\n'-terminated line, stripping an optional preceding '\r'.
 *
 * On RING_LINE_OK the full line content is copied to `line_buffer` and
 * *line_length (if non-NULL) is set to its length (excluding CR/LF).
 * On RING_LINE_TOO_LONG the line still does NOT fit: a truncated prefix is
 * copied, *line_length reports the true (full) content length, and the whole
 * line is consumed so the caller cannot accidentally re-process it.
 * On RING_LINE_NONE the buffer is left untouched.
 */
RingLineResult ring_buffer_readline(RingBuffer *rb, char *line_buffer,
                                    size_t line_buffer_size, size_t *line_length);

/*
 * Development/testing invariant check.  Verifies the structure is internally
 * consistent (non-NULL storage, 0 <= size <= capacity, head/tail in range, and
 * head == (tail + size) % capacity).  Returns 1 when valid, 0 otherwise.
 * Cheap enough to assert after mutations in debug builds.
 */
int ring_buffer_validate(const RingBuffer *rb);

#endif
