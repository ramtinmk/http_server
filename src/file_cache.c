#include "file_cache.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CACHE_KEY_MAX 1024

struct CacheEntry {
    FileCache          *owner;
    char                key[CACHE_KEY_MAX];
    Representation      plain;
    Representation      gzip;
    size_t              bytes;   /* plain.body_len + gzip.body_len */
    int                 refs;
    int                 evicted; /* free as soon as refs reaches 0 */
    struct CacheEntry  *prev;    /* LRU: head = most recently used */
    struct CacheEntry  *next;
};

struct FileCache {
    pthread_mutex_t  lock;
    struct CacheEntry *head;
    struct CacheEntry *tail;
    size_t           bytes;
    size_t           entries;
    size_t           budget;
    size_t           max_entries;
};

static void entry_free(struct CacheEntry *e)
{
    free(e->plain.body);
    free(e->gzip.body);
    free(e);
}

static void lru_unlink(FileCache *c, struct CacheEntry *e)
{
    if (e->prev)
        e->prev->next = e->next;
    else
        c->head = e->next;
    if (e->next)
        e->next->prev = e->prev;
    else
        c->tail = e->prev;
    e->prev = e->next = NULL;
}

static void lru_push_front(FileCache *c, struct CacheEntry *e)
{
    e->prev = NULL;
    e->next = c->head;
    if (c->head)
        c->head->prev = e;
    c->head = e;
    if (!c->tail)
        c->tail = e;
}

static void lru_touch(FileCache *c, struct CacheEntry *e)
{
    if (c->head == e)
        return;
    lru_unlink(c, e);
    lru_push_front(c, e);
}

static void cache_remove(FileCache *c, struct CacheEntry *e)
{
    lru_unlink(c, e);
    c->bytes -= e->bytes;
    c->entries--;
    entry_free(e);
}

/* Remove the least-recently-used entry with no live references. Returns 0 when
 * one was removed, -1 when every entry is pinned. */
static int evict_unreferenced_tail(FileCache *c)
{
    struct CacheEntry *victim = c->tail;
    while (victim && victim->refs > 0)
        victim = victim->prev;
    if (!victim)
        return -1;
    cache_remove(c, victim);
    return 0;
}

/* Evict unreferenced LRU entries until room exists for `need` bytes. Returns 0
 * when room was made, -1 when the budget cannot hold the addition. */
static int make_room(FileCache *c, size_t need)
{
    if (need > c->budget)
        return -1;
    while (c->bytes + need > c->budget ||
           (c->max_entries && c->entries + 1 > c->max_entries)) {
        if (evict_unreferenced_tail(c) != 0)
            return -1; /* everything pinned */
    }
    return 0;
}

static struct CacheEntry *cache_find(FileCache *c, const char *key)
{
    for (struct CacheEntry *e = c->head; e; e = e->next) {
        if (strcmp(e->key, key) == 0)
            return e;
    }
    return NULL;
}

FileCache *file_cache_create(size_t budget_bytes, size_t max_entries)
{
    FileCache *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    if (pthread_mutex_init(&c->lock, NULL) != 0) {
        free(c);
        return NULL;
    }
    c->budget = budget_bytes;
    c->max_entries = max_entries;
    return c;
}

void file_cache_destroy(FileCache *c)
{
    if (!c)
        return;
    struct CacheEntry *e = c->head;
    while (e) {
        struct CacheEntry *next = e->next;
        entry_free(e);
        e = next;
    }
    pthread_mutex_destroy(&c->lock);
    free(c);
}

void file_cache_set_budget(FileCache *c, size_t budget_bytes)
{
    if (!c)
        return;
    pthread_mutex_lock(&c->lock);
    c->budget = budget_bytes;
    while (c->bytes > c->budget) {
        if (evict_unreferenced_tail(c) != 0)
            break;
    }
    pthread_mutex_unlock(&c->lock);
}

size_t file_cache_bytes(const FileCache *c)
{
    return c ? c->bytes : 0;
}

size_t file_cache_entry_count(const FileCache *c)
{
    return c ? c->entries : 0;
}

struct CacheEntry *file_cache_acquire(FileCache *c, const char *key)
{
    if (!c)
        return NULL;
    pthread_mutex_lock(&c->lock);
    struct CacheEntry *e = cache_find(c, key);
    if (e) {
        e->refs++;
        lru_touch(c, e);
    }
    pthread_mutex_unlock(&c->lock);
    return e;
}

const Representation *file_cache_plain(const struct CacheEntry *e)
{
    return &e->plain;
}

const Representation *file_cache_gzip(const struct CacheEntry *e)
{
    return &e->gzip;
}

void file_cache_release(struct CacheEntry *e)
{
    if (!e)
        return;
    FileCache *c = e->owner;
    pthread_mutex_lock(&c->lock);
    if (--e->refs == 0 && e->evicted)
        cache_remove(c, e);
    pthread_mutex_unlock(&c->lock);
}

struct CacheEntry *file_cache_insert(FileCache *c, const char *key,
                                     const Representation *plain,
                                     const Representation *gzip)
{
    if (!c || c->budget == 0)
        return NULL;

    size_t bytes = plain->body_len + gzip->body_len;

    pthread_mutex_lock(&c->lock);

    struct CacheEntry *existing = cache_find(c, key);
    if (existing) {
        existing->refs++;
        lru_touch(c, existing);
        pthread_mutex_unlock(&c->lock);
        free(plain->body);
        free(gzip->body);
        return existing;
    }

    if (strlen(key) >= CACHE_KEY_MAX || make_room(c, bytes) != 0) {
        pthread_mutex_unlock(&c->lock);
        return NULL;
    }

    struct CacheEntry *e = calloc(1, sizeof(*e));
    if (!e) {
        pthread_mutex_unlock(&c->lock);
        return NULL;
    }
    e->owner = c;
    snprintf(e->key, sizeof(e->key), "%s", key);
    e->plain = *plain;
    e->gzip = *gzip;
    e->plain.fd = -1;
    e->gzip.fd = -1;
    e->bytes = bytes;
    e->refs = 1;
    lru_push_front(c, e);
    c->bytes += bytes;
    c->entries++;
    pthread_mutex_unlock(&c->lock);
    return e;
}
