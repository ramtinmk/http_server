#include "config.h"
#include "file_cache.h"
#include "http_server.h"
#include "ring_buffer.h"

#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>

static int g_initialized;

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc;
    (void)argv;
    ServerConfig cfg;
    config_defaults(&cfg);
    if (initialize_static_responses(&cfg) != 0)
        return -1;
    g_initialized = 1;
    return 0;
}

static void release_response(PendingResponse *response)
{
    free(response->owned_body);
    response->owned_body = NULL;
    if (response->body_fd >= 0)
        close(response->body_fd);
    response->body_fd = -1;
    file_cache_release((struct CacheEntry *)response->cache_entry);
    response->cache_entry = NULL;
}

/* Feed arbitrary fragments through the same transactional parser/queue rules
 * used by the event loop without creating sockets or threads per fuzz input. */
int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
    if (!g_initialized || size > (1u << 20))
        return 0;

    RingBuffer *buffer = ring_buffer_create(INITIAL_RING_BUFFER_CAPACITY);
    if (!buffer)
        return 0;
    size_t offset = 0;
    int request_count = 0;
    int queue_depth = 0;
    int reading = 1;
    while (offset < size && request_count < MAX_KEEPALIVE_REQUESTS) {
        size_t fragment = 1 + (data[offset] % 97);
        if (fragment > size - offset)
            fragment = size - offset;
        (void)ring_buffer_write(buffer, (const char *)data + offset, fragment);
        offset += fragment;

        while (reading && queue_depth < MAX_PIPELINE_DEPTH &&
               !ring_buffer_is_empty(buffer)) {
            PendingResponse response;
            int keep_alive = 0;
            int force_close = request_count + 1 >= MAX_KEEPALIVE_REQUESTS;
            int result = el_prepare_response(buffer, force_close, &keep_alive,
                                             &response);
            if (result == 1)
                break;
            if (result != 0) {
                reading = 0;
                break;
            }
            request_count++;
            queue_depth++;
            if (!keep_alive)
                reading = 0;

            /* Model a writable event draining one response at a time. */
            release_response(&response);
            queue_depth--;
        }
    }
    ring_buffer_free(buffer);
    return 0;
}
