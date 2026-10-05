/*
 * Differential harness (C side).
 *
 * Drives the real `src/ring_buffer.c` through the same bounded family of
 * `create -> write -> read -> read` sequences as the Lean model in
 * `verification/lean/RingBufferDiff.lean` and prints
 * `capacity head tail size valid` one case per line.
 *
 * `verification/run.sh` diffs the two tables. The input sizes are chosen so the
 * exponential-growth resize path is never taken (`s <= capacity`), keeping the
 * comparison scoped to the cursor arithmetic that the Lean model covers.
 */
#include "ring_buffer.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    for (size_t c = 1; c <= 16; c++) {
        for (size_t s = 0; s <= c; s++) {
            for (size_t r = 0; r <= s; r++) {
                for (size_t len = 0; len <= s - r; len++) {
                    RingBuffer *rb = ring_buffer_create(c);
                    if (!rb) {
                        fprintf(stderr, "ring_buffer_create(%zu) failed\n", c);
                        return 1;
                    }

                    char source[16] = {0};
                    char dest[16] = {0};
                    if (s > 0) {
                        ring_buffer_write(rb, source, s);
                    }
                    if (r > 0) {
                        ring_buffer_read(rb, dest, r);
                    }
                    if (len > 0) {
                        ring_buffer_read(rb, dest, len);
                    }

                    printf("%zu %zu %zu %zu %d\n", rb->capacity, rb->head,
                           rb->tail, rb->size, ring_buffer_validate(rb));
                    ring_buffer_free(rb);
                }
            }
        }
    }
    return 0;
}
