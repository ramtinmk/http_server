#define _GNU_SOURCE

#include "memory_profiler.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

/*
 * The sample is refreshed by the reporter thread and read by the snapshot
 * formatter on the same thread, so relaxed atomics are enough: the values are
 * independent statistics where a slightly stale read is acceptable. High-water
 * marks use the same CAS pattern as the buffer counters in metrics.c.
 */

static _Atomic long g_rss_kb;
static _Atomic long g_rss_kb_max;
static _Atomic long g_pss_kb;
static _Atomic long g_pss_kb_max;
static _Atomic long g_private_dirty_kb;
static _Atomic long g_vmsize_kb;
static _Atomic long g_heap_inuse_bytes;
static _Atomic long g_heap_inuse_bytes_max;
static _Atomic long g_heap_mmap_bytes;
static _Atomic int  g_sample_ok;

/* --- /proc readers ------------------------------------------------------- */

/* statm reports page counts; convert to kB using the runtime page size. */
static long pages_to_kb(long pages)
{
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        page_size = 4096;
    }
    return pages * (page_size / 1024);
}

/* Read virtual and resident size from /proc/self/statm. Returns nonzero on
 * success. The first two fields are size and resident, both in pages. */
static int read_statm(long *vmsize_kb, long *rss_kb)
{
    FILE *fp = fopen("/proc/self/statm", "r");
    if (!fp) {
        return 0;
    }
    char line[256];
    int ok = 0;
    if (fgets(line, sizeof(line), fp)) {
        char *after_size = NULL;
        long size = strtol(line, &after_size, 10);
        char *after_resident = NULL;
        long resident = strtol(after_size, &after_resident, 10);
        if (after_resident != after_size) {
            *vmsize_kb = pages_to_kb(size);
            *rss_kb = pages_to_kb(resident);
            ok = 1;
        }
    }
    fclose(fp);
    return ok;
}

/* Read Pss and Private_Dirty from /proc/self/smaps_rollup in one pass.
 * Private_Dirty counts anonymous resident pages that back only this process; it
 * is a stabler heap proxy than mallinfo's ratcheting in-use figure. Returns
 * nonzero when Pss was found. The file is optional: older kernels and some
 * containers do not expose it, in which case both stay reported as zero. */
static int read_smaps_rollup(long *pss_kb, long *private_dirty_kb)
{
    FILE *fp = fopen("/proc/self/smaps_rollup", "r");
    if (!fp) {
        return 0;
    }
    char line[256];
    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "Pss:", 4) == 0) {
            char *end = NULL;
            long value = strtol(line + 4, &end, 10);
            if (end != line + 4) {
                *pss_kb = value;
                found = 1;
            }
        } else if (strncmp(line, "Private_Dirty:", 14) == 0) {
            char *end = NULL;
            long value = strtol(line + 14, &end, 10);
            if (end != line + 14) {
                *private_dirty_kb = value;
            }
        }
    }
    fclose(fp);
    return found;
}

/* Read glibc heap accounting. `uordblks` is total space in use by the
 * allocator; `hblkhd` is space in mmapped regions. mallinfo2() (glibc 2.33+)
 * avoids mallinfo()'s narrow int fields; on older glibc we fall back to
 * mallinfo(). Returns 0 on non-glibc platforms. */
static int read_heap(long *inuse_bytes, long *mmap_bytes)
{
#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 33)
    struct mallinfo2 mi = mallinfo2();
#else
    struct mallinfo mi = mallinfo();
#endif
    *inuse_bytes = (long)mi.uordblks;
    *mmap_bytes = (long)mi.hblkhd;
    return 1;
#else
    (void)inuse_bytes;
    (void)mmap_bytes;
    return 0;
#endif
}

/* --- Sample and high-water ---------------------------------------------- */

static void update_max(_Atomic long *max, long value)
{
    long observed = atomic_load_explicit(max, memory_order_relaxed);
    while (value > observed &&
           !atomic_compare_exchange_weak_explicit(
               max, &observed, value, memory_order_relaxed,
               memory_order_relaxed)) {
    }
}

void memory_profiler_sample(void)
{
    long vmsize_kb = 0;
    long rss_kb = 0;
    long pss_kb = 0;
    long private_dirty_kb = 0;
    long heap_inuse = 0;
    long heap_mmap = 0;

    int statm_ok = read_statm(&vmsize_kb, &rss_kb);
    /* optional; leaves both 0 when smaps_rollup is unavailable */
    read_smaps_rollup(&pss_kb, &private_dirty_kb);
    read_heap(&heap_inuse, &heap_mmap);

    atomic_store_explicit(&g_vmsize_kb, vmsize_kb, memory_order_relaxed);
    atomic_store_explicit(&g_rss_kb, rss_kb, memory_order_relaxed);
    atomic_store_explicit(&g_pss_kb, pss_kb, memory_order_relaxed);
    atomic_store_explicit(&g_private_dirty_kb, private_dirty_kb,
                          memory_order_relaxed);
    atomic_store_explicit(&g_heap_inuse_bytes, heap_inuse, memory_order_relaxed);
    atomic_store_explicit(&g_heap_mmap_bytes, heap_mmap, memory_order_relaxed);
    atomic_store_explicit(&g_sample_ok, statm_ok, memory_order_relaxed);

    update_max(&g_rss_kb_max, rss_kb);
    update_max(&g_pss_kb_max, pss_kb);
    update_max(&g_heap_inuse_bytes_max, heap_inuse);
}

/* --- Snapshot rendering -------------------------------------------------- */

#define LOAD(counter) atomic_load_explicit(&(counter), memory_order_relaxed)

size_t memory_profiler_append_json(char *buf, size_t cap, size_t off)
{
    /* Render into a local fragment first so the untruncated length is known
     * even when the destination is already full. */
    char fragment[448];
    int n = snprintf(
        fragment, sizeof(fragment),
        ",\"rss_kb\":%ld,\"rss_kb_max\":%ld,"
        "\"pss_kb\":%ld,\"pss_kb_max\":%ld,"
        "\"private_dirty_kb\":%ld,"
        "\"vmsize_kb\":%ld,"
        "\"heap_inuse_bytes\":%ld,\"heap_inuse_bytes_max\":%ld,"
        "\"heap_mmap_bytes\":%ld,"
        "\"memory_sample_ok\":%d",
        LOAD(g_rss_kb), LOAD(g_rss_kb_max),
        LOAD(g_pss_kb), LOAD(g_pss_kb_max),
        LOAD(g_private_dirty_kb),
        LOAD(g_vmsize_kb),
        LOAD(g_heap_inuse_bytes), LOAD(g_heap_inuse_bytes_max),
        LOAD(g_heap_mmap_bytes),
        LOAD(g_sample_ok));
    if (n < 0) {
        return off;
    }
    if (off < cap) {
        size_t room = cap - off;
        size_t copy = (size_t)n < room ? (size_t)n : room - 1;
        memcpy(buf + off, fragment, copy);
        buf[off + copy] = '\0';
    }
    return off + (size_t)n;
}

#undef LOAD
