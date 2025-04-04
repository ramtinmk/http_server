#include "ring_buffer.h"


// --- Ring Buffer Function Implementations ---
static int ring_buffer_resize(RingBuffer *rb, size_t new_capacity)
{
    if (!rb || new_capacity <= rb->capacity || new_capacity < rb->size)
    {
        // Invalid arguments or no resize needed/possible
        return -1; // Or maybe 0 if new_capacity <= capacity? Let's stick to -1 for error.
    }

    // Optional: Check against a maximum capacity limit
    /*
    if (new_capacity > MAX_RING_BUFFER_CAPACITY) {
        fprintf(stderr, "Error: RingBuffer resize requested capacity %zu exceeds maximum %d\n",
                new_capacity, MAX_RING_BUFFER_CAPACITY);
        errno = ENOMEM; // Indicate memory limit reached
        return -1;
    }
    */

    char *new_buffer = malloc(new_capacity);
    if (!new_buffer)
    {
        perror("Failed to allocate memory for RingBuffer resize");
        return -1;
    }

    // Copy data from old buffer to new buffer, linearizing it
    size_t bytes_to_copy_part1 = 0;
    size_t bytes_to_copy_part2 = 0;

    if (rb->size > 0)
    {
        if (rb->head > rb->tail)
        {
            // Data is contiguous
            bytes_to_copy_part1 = rb->size;
            memcpy(new_buffer, rb->buffer + rb->tail, bytes_to_copy_part1);
        }
        else
        {
            // Data wraps around
            bytes_to_copy_part1 = rb->capacity - rb->tail;
            memcpy(new_buffer, rb->buffer + rb->tail, bytes_to_copy_part1);

            bytes_to_copy_part2 = rb->head;
            memcpy(new_buffer + bytes_to_copy_part1, rb->buffer, bytes_to_copy_part2);
        }
    }

    // Free the old buffer
    free(rb->buffer);

    // Update RingBuffer structure
    rb->buffer = new_buffer;
    rb->capacity = new_capacity;
    rb->tail = 0;        // Data is now linear, starting at index 0
    rb->head = rb->size; // Head is positioned after the last byte of existing data

    // fprintf(stderr, "DEBUG: RingBuffer resized to capacity %zu\n", new_capacity); // Optional debug print

    return 0; // Success
}

RingBuffer *ring_buffer_create(size_t initial_capacity)
{
    // Use INITIAL_RING_BUFFER_CAPACITY if initial_capacity is 0 or too small
    if (initial_capacity == 0)
        initial_capacity = INITIAL_RING_BUFFER_CAPACITY;

    RingBuffer *rb = malloc(sizeof(RingBuffer));
    if (!rb)
    {
        perror("Failed to allocate RingBuffer struct");
        return NULL;
    }
    rb->buffer = malloc(initial_capacity);
    if (!rb->buffer)
    {
        perror("Failed to allocate RingBuffer internal buffer");
        free(rb);
        return NULL;
    }
    rb->capacity = initial_capacity;
    ring_buffer_reset(rb); // Initialize head, tail, size
    return rb;
}

// --- Modified ring_buffer_write ---
size_t ring_buffer_write(RingBuffer *rb, const char *data, size_t data_len)
{
    if (!rb || !data || data_len == 0)
        return 0;

    size_t available_space = rb->capacity - rb->size;

    // Check if resizing is needed
    if (data_len > available_space)
    {
        // Calculate new capacity: at least double, but ensure enough space for new data
        size_t needed_capacity = rb->size + data_len;
        size_t new_capacity = rb->capacity;
        do
        {
            new_capacity *= 2; // Double the capacity
            // Handle potential overflow if capacity becomes huge, though unlikely with size_t
            if (new_capacity < rb->capacity)
            {                                   // Check for overflow
                new_capacity = needed_capacity; // Fallback if doubling overflows
                if (new_capacity < rb->size || new_capacity < data_len)
                { // Check needed_capacity didn't overflow
                    fprintf(stderr, "Error: RingBuffer capacity overflow during resize calculation.\n");
                    errno = EOVERFLOW;
                    return 0; // Cannot resize sufficiently
                }
                // If needed_capacity is valid but doubling overflowed, use needed_capacity if it's larger than current capacity
                if (new_capacity <= rb->capacity)
                {
                    fprintf(stderr, "Error: RingBuffer cannot grow large enough.\n");
                    errno = ENOMEM;
                    return 0;
                }
                break; // Use the calculated needed_capacity (if valid & larger)
            }
        } while (new_capacity < needed_capacity);

        // Attempt to resize
        if (ring_buffer_resize(rb, new_capacity) != 0)
        {
            // Resizing failed (e.g., out of memory)
            // Try to write whatever fits in the *current* available space
            size_t bytes_to_write = available_space;
            if (bytes_to_write == 0)
                return 0; // Buffer is completely full, resize failed

            // Proceed with writing partial data (existing logic)
            size_t available_space_to_end = rb->capacity - rb->head;

            if (bytes_to_write <= available_space_to_end)
            {
                memcpy(rb->buffer + rb->head, data, bytes_to_write);
                rb->head = (rb->head + bytes_to_write) % rb->capacity; // Use modulo for safety
            }
            else
            {
                memcpy(rb->buffer + rb->head, data, available_space_to_end);
                memcpy(rb->buffer, data + available_space_to_end, bytes_to_write - available_space_to_end);
                rb->head = bytes_to_write - available_space_to_end;
            }
            rb->size += bytes_to_write;
            fprintf(stderr, "Warning: RingBuffer resize failed, wrote partial data (%zu bytes)\n", bytes_to_write);
            return bytes_to_write; // Return the amount actually written
        }
        // Resizing succeeded, available_space is now updated implicitly by the change in rb->capacity
        // The write logic below will now handle the full data_len
    }

    // --- Original write logic (now guaranteed to have enough space) ---
    size_t bytes_to_write = data_len; // We know we have space now

    size_t available_space_to_end = rb->capacity - rb->head;

    if (bytes_to_write <= available_space_to_end)
    {
        memcpy(rb->buffer + rb->head, data, bytes_to_write);
        rb->head += bytes_to_write;
        if (rb->head == rb->capacity)
        { // Wrap around if head reaches end
            rb->head = 0;
        }
    }
    else
    { // Data wraps around
        memcpy(rb->buffer + rb->head, data, available_space_to_end);
        memcpy(rb->buffer, data + available_space_to_end, bytes_to_write - available_space_to_end);
        rb->head = bytes_to_write - available_space_to_end;
    }
    rb->size += bytes_to_write;
    return bytes_to_write;
}

void ring_buffer_free(RingBuffer *rb)
{
    if (!rb)
        return;
    free(rb->buffer);
    free(rb);
}

void ring_buffer_reset(RingBuffer *rb)
{
    if (!rb)
        return;
    rb->head = 0;
    rb->tail = 0;
    rb->size = 0;
}

size_t ring_buffer_get_size(const RingBuffer *rb)
{
    return rb ? rb->size : 0;
}

size_t ring_buffer_get_capacity(const RingBuffer *rb)
{
    return rb ? rb->capacity : 0;
}
int ring_buffer_is_empty(const RingBuffer *rb)
{
    return rb ? rb->size == 0 : 1;
}

int ring_buffer_is_full(const RingBuffer *rb)
{
    return rb ? rb->size == rb->capacity : 0;
}

size_t ring_buffer_read(RingBuffer *rb, char *dest, size_t dest_len)
{
    if (!rb || !dest || dest_len == 0 || ring_buffer_is_empty(rb))
        return 0;

    size_t bytes_to_read = dest_len;
    if (bytes_to_read > rb->size)
    {
        bytes_to_read = rb->size; // Don't read more than what's in buffer
    }
    if (bytes_to_read == 0)
        return 0;

    size_t available_data_to_end = rb->capacity - rb->tail;

    if (bytes_to_read <= available_data_to_end)
    {
        memcpy(dest, rb->buffer + rb->tail, bytes_to_read);
        rb->tail += bytes_to_read;
        if (rb->tail == rb->capacity)
        { // Wrap around if tail reaches end
            rb->tail = 0;
        }
    }
    else
    { // Data wraps around
        memcpy(dest, rb->buffer + rb->tail, available_data_to_end);
        memcpy(dest + available_data_to_end, rb->buffer, bytes_to_read - available_data_to_end);
        rb->tail = bytes_to_read - available_data_to_end;
    }
    rb->size -= bytes_to_read;
    return bytes_to_read;
}

size_t ring_buffer_peek(const RingBuffer *rb, char *dest, size_t dest_len)
{
    if (!rb || !dest || dest_len == 0 || ring_buffer_is_empty(rb))
        return 0;

    size_t bytes_to_peek = dest_len;
    if (bytes_to_peek > rb->size)
    {
        bytes_to_peek = rb->size;
    }
    if (bytes_to_peek == 0)
        return 0;

    size_t available_data_to_end = rb->capacity - rb->tail;

    if (bytes_to_peek <= available_data_to_end)
    {
        memcpy(dest, rb->buffer + rb->tail, bytes_to_peek);
        // Do not advance tail for peek operation
    }
    else
    {
        memcpy(dest, rb->buffer + rb->tail, available_data_to_end);
        memcpy(dest + available_data_to_end, rb->buffer, bytes_to_peek - available_data_to_end);
    }

    return bytes_to_peek; // Return how many bytes we peeked
}

char *ring_buffer_readline(RingBuffer *rb, char *line_buffer, size_t line_buffer_size)
{
    // --- Argument Validation ---
    if (!rb || !line_buffer || line_buffer_size == 0)
    {
        return NULL; // Invalid arguments
    }
    line_buffer[0] = '\0'; // Ensure buffer is empty initially or on early return

    size_t bytes_in_rb = ring_buffer_get_size(rb);
    if (bytes_in_rb == 0)
    {
        return NULL; // Empty buffer, no line possible
    }

    // --- Efficiently Find Newline Offset ---
    size_t newline_offset = (size_t)-1; // Sentinel for not found
    for (size_t i = 0; i < bytes_in_rb; ++i)
    {
        // Calculate index without modifying rb->tail yet
        size_t current_idx = (rb->tail + i) % rb->capacity;
        if (rb->buffer[current_idx] == '\n')
        {
            newline_offset = i;
            break;
        }
    }

    // If no newline was found in the available data
    if (newline_offset == (size_t)-1)
    {
        return NULL;
    }

    // --- Determine Actual Line Length and Ending Type ---
    // newline_offset is the index relative to tail where '\n' is.
    size_t actual_line_len = newline_offset; // Length initially excludes \n
    int cr_found = 0;
    size_t ending_len = 1; // Bytes to consume for line ending (\n)

    // Check if the character *before* '\n' is '\r'
    if (newline_offset > 0)
    {
        size_t prev_idx = (rb->tail + newline_offset - 1) % rb->capacity;
        if (rb->buffer[prev_idx] == '\r')
        {
            cr_found = 1;
            actual_line_len = newline_offset - 1; // Actual line content excludes \r too
            ending_len = 2;                       // Line ending is \r\n
        }
    }

    // --- Read Actual Line Content (up to buffer size limit) ---
    size_t len_to_copy = actual_line_len;
    if (len_to_copy >= line_buffer_size)
    {
        // fprintf(stderr, "Warning: ring_buffer_readline truncated line (len %zu) to fit buffer (size %zu)\n",
        //         actual_line_len, line_buffer_size);
        len_to_copy = line_buffer_size - 1; // Leave space for null terminator
    }

    // Use ring_buffer_read to get the actual line content.
    // This advances rb->tail past the content.
    size_t bytes_read = ring_buffer_read(rb, line_buffer, len_to_copy);
    if (bytes_read != len_to_copy)
    {
        // This indicates an internal error in ring_buffer_read or the logic here.
        fprintf(stderr, "Error: ring_buffer_readline failed to read expected line content (%zu != %zu).\n", bytes_read, len_to_copy);
        // State is potentially inconsistent. Returning NULL is safest.
        // We might have partially read data, leaving the buffer state difficult to recover.
        return NULL;
    }
    line_buffer[len_to_copy] = '\0'; // Null-terminate the string in the destination buffer.

    // --- Consume Remaining Part of Line (if truncated) and the Line Ending ---

    // Bytes of the *actual line* that were not copied because the buffer was too small
    size_t remaining_line_bytes_to_discard = actual_line_len - len_to_copy;

    // Total bytes remaining in the buffer that belong to this line (truncated part + ending)
    size_t total_bytes_to_consume = remaining_line_bytes_to_discard + ending_len;

    // Consume these bytes efficiently
    if (total_bytes_to_consume > 0)
    {
        // Optimization: Can we just advance head/tail directly if possible?
        // ring_buffer_read internally advances tail and decreases size.
        // We can just read into a small discard buffer or potentially optimize within ring_buffer_read itself if needed.
        char discard_buffer[16]; // Small temporary buffer
        size_t consumed_count = 0;
        while (consumed_count < total_bytes_to_consume)
        {
            size_t amount_to_consume_now = total_bytes_to_consume - consumed_count;
            if (amount_to_consume_now > sizeof(discard_buffer))
            {
                amount_to_consume_now = sizeof(discard_buffer);
            }
            size_t just_consumed = ring_buffer_read(rb, discard_buffer, amount_to_consume_now);

            // If ring_buffer_read returns 0 before consuming all expected bytes,
            // it means the buffer became empty unexpectedly (error).
            if (just_consumed == 0 && (consumed_count < total_bytes_to_consume))
            {
                fprintf(stderr, "Error: ring_buffer_readline buffer became empty while consuming line remainder/ending (consumed %zu/%zu).\n",
                        consumed_count, total_bytes_to_consume);
                // The buffer state is now likely corrupt relative to expectations.
                return NULL; // Indicate error
            }
            consumed_count += just_consumed;
        }
        // Optional check: Verify total consumed amount
        if (consumed_count != total_bytes_to_consume)
        {
            fprintf(stderr, "Warning: ring_buffer_readline consumed %zu bytes, expected to consume %zu for remainder+ending.\n",
                    consumed_count, total_bytes_to_consume);
            // Proceed, but log this potential issue.
        }
    }

    return line_buffer; // Success
}