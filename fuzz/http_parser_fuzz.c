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
    /* Parser fuzzing needs the same startup cache used by a real request. */
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

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
    if (!g_initialized || size > (1u << 20))
        return 0;

    RingBuffer *buffer = ring_buffer_create(INITIAL_RING_BUFFER_CAPACITY);
    if (!buffer)
        return 0;
    (void)ring_buffer_write(buffer, (const char *)data, size);

    for (int request = 0; request < MAX_PIPELINE_DEPTH + 1 &&
                           !ring_buffer_is_empty(buffer); request++) {
        PendingResponse response;
        int keep_alive = 0;
        int result = el_prepare_response(buffer, request == MAX_PIPELINE_DEPTH,
                                         &keep_alive, &response);
        if (result == 0)
            release_response(&response);
        else
            break;
    }
    ring_buffer_free(buffer);
    return 0;
}
