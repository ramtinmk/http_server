#define _GNU_SOURCE
#include "client_limits.h"
#include "server_config.h"

#include <pthread.h>
#include <string.h>
#include <time.h>

typedef struct {
    char peer[64];
    unsigned int active_connections;
    unsigned int requests_in_window;
    time_t window_start;
    unsigned long long last_used;
    int occupied;
} ClientEntry;

static ClientEntry g_entries[PER_IP_LIMIT_TABLE_SIZE];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned int g_connection_limit;
static unsigned int g_request_limit;
static unsigned long long g_clock;

int client_limits_init(const ServerConfig *cfg)
{
    pthread_mutex_lock(&g_lock);
    memset(g_entries, 0, sizeof(g_entries));
    g_connection_limit = (unsigned int)cfg->per_ip_connections;
    g_request_limit = (unsigned int)cfg->per_ip_requests_per_minute;
    g_clock = 0;
    pthread_mutex_unlock(&g_lock);
    return 0;
}

/* Find an existing peer or allocate a free/LRU inactive slot. Caller holds lock. */
static ClientEntry *entry_get(const char *peer, int create)
{
    ClientEntry *free_entry = NULL;
    ClientEntry *oldest = NULL;
    for (size_t i = 0; i < PER_IP_LIMIT_TABLE_SIZE; i++) {
        ClientEntry *entry = &g_entries[i];
        if (entry->occupied && strcmp(entry->peer, peer) == 0)
            return entry;
        if (!entry->occupied && !free_entry)
            free_entry = entry;
        if (entry->occupied && entry->active_connections == 0 &&
            (!oldest || entry->last_used < oldest->last_used))
            oldest = entry;
    }
    if (!create)
        return NULL;
    ClientEntry *entry = free_entry ? free_entry : oldest;
    if (!entry)
        return NULL;
    memset(entry, 0, sizeof(*entry));
    size_t n = strnlen(peer, sizeof(entry->peer));
    if (n >= sizeof(entry->peer))
        return NULL;
    memcpy(entry->peer, peer, n + 1);
    entry->occupied = 1;
    return entry;
}

int client_limits_connection_admit(const char *peer)
{
    if (g_connection_limit == 0 && g_request_limit == 0)
        return 1;
    pthread_mutex_lock(&g_lock);
    ClientEntry *entry = entry_get(peer, 1);
    int allowed = entry != NULL &&
                  (g_connection_limit == 0 ||
                   entry->active_connections < g_connection_limit);
    if (allowed) {
        entry->active_connections++;
        entry->last_used = ++g_clock;
    }
    pthread_mutex_unlock(&g_lock);
    return allowed;
}

void client_limits_connection_release(const char *peer)
{
    if (g_connection_limit == 0 && g_request_limit == 0)
        return;
    pthread_mutex_lock(&g_lock);
    ClientEntry *entry = entry_get(peer, 0);
    if (entry && entry->active_connections > 0) {
        entry->active_connections--;
        entry->last_used = ++g_clock;
    }
    pthread_mutex_unlock(&g_lock);
}

int client_limits_request_admit(const char *peer)
{
    if (g_request_limit == 0)
        return 1;
    time_t now = time(NULL);
    pthread_mutex_lock(&g_lock);
    ClientEntry *entry = entry_get(peer, 1);
    int allowed = entry != NULL;
    if (entry) {
        if (entry->window_start == 0 || now - entry->window_start >= 60 ||
            now < entry->window_start) {
            entry->window_start = now;
            entry->requests_in_window = 0;
        }
        allowed = entry->requests_in_window < g_request_limit;
        if (allowed)
            entry->requests_in_window++;
        entry->last_used = ++g_clock;
    }
    pthread_mutex_unlock(&g_lock);
    return allowed;
}
