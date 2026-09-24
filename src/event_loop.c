#define _GNU_SOURCE

#include "event_loop.h"
#include "metrics.h"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* ------------------------------------------------------------------ */
/* Local constants                                                      */
/* ------------------------------------------------------------------ */
#define EL_RECV_BUFSIZE 8192

/*
 * Complete, self-framed overload response. The body length is filled in once
 * during startup so the header and body never disagree. Returned when a
 * connection is admitted past the capacity check (a race between loops) so the
 * client sees a bounded HTTP response rather than a silent reset.
 */
static char   g_overload_response[512];
static size_t g_overload_len;

static void build_overload_response(void)
{
    if (g_overload_len)
        return;
    static const char body[] =
        "<html><head><title>503 Service Unavailable</title></head>"
        "<body><h1>503 Service Unavailable</h1>"
        "<p>Server at connection capacity</p></body></html>";
    int n = snprintf(g_overload_response, sizeof(g_overload_response),
                     "HTTP/1.1 503 Service Unavailable\r\n"
                     "Server: SimpleHTTPServer/1.0\r\n"
                     "Connection: close\r\n"
                     "Content-Type: text/html\r\n"
                     "Content-Length: %zu\r\n"
                     "\r\n"
                     "%s",
                     sizeof(body) - 1, body);
    g_overload_len = (n > 0 && (size_t)n < sizeof(g_overload_response))
                         ? (size_t)n : 0;
}

/* ------------------------------------------------------------------ */
/* Per-loop state                                                       */
/* ------------------------------------------------------------------ */
struct EventLoop {
    int              id;                  /* 0-based loop index (diagnostics)  */
    int              epoll_fd;
    int              listen_fd;
    int              listen_enabled;      /* EPOLLIN currently registered?     */
    int              wake_fd;             /* eventfd used to break epoll_wait  */
    ELConnection     listen_sentinel;     /* epoll tag for the listener        */
    ELConnection     wake_sentinel;       /* epoll tag for the wake eventfd    */

    ELConnection    *pool;                /* Runtime-sized connection table    */
    size_t           pool_size;
    ELConnection    *free_list;

    long             capacity;            /* Process-wide active-connection cap*/
    volatile sig_atomic_t *running;

    long             listener_disabled_at_ms; /* -1 while listener is enabled */

    /* Per-loop diagnostics, reported at shutdown alongside the aggregate
     * metrics snapshot so accept distribution across loops is observable. */
    unsigned long long wakeups;
    unsigned long long accepted;
};

/* ------------------------------------------------------------------ */
/* Clock helpers                                                        */
/* ------------------------------------------------------------------ */
static void now_mono(struct timespec *ts)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
}

static long now_ms(void)
{
    struct timespec ts;
    now_mono(&ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void deadline_set(struct timespec *dl, int seconds)
{
    now_mono(dl);
    dl->tv_sec += seconds;
}

static int deadline_expired(const struct timespec *dl)
{
    struct timespec now;
    now_mono(&now);
    return now.tv_sec > dl->tv_sec ||
           (now.tv_sec == dl->tv_sec && now.tv_nsec >= dl->tv_nsec);
}

/* ------------------------------------------------------------------ */
/* Connection table                                                     */
/* ------------------------------------------------------------------ */
static int pool_init(EventLoop *loop, size_t size)
{
    loop->pool = calloc(size ? size : 1, sizeof(ELConnection));
    if (!loop->pool)
        return -1;
    loop->pool_size = size;
    loop->free_list = NULL;
    for (size_t i = size; i-- > 0; ) {
        loop->pool[i].fd   = -1;
        loop->pool[i].next = loop->free_list;
        loop->free_list    = &loop->pool[i];
    }
    return 0;
}

static ELConnection *conn_alloc(EventLoop *loop)
{
    if (!loop->free_list)
        return NULL;
    ELConnection *c = loop->free_list;
    loop->free_list = c->next;
    memset(c, 0, sizeof(*c));
    c->fd   = -1;
    c->next = NULL;
    return c;
}

static void conn_return(EventLoop *loop, ELConnection *c)
{
    c->fd   = -1;
    c->next = loop->free_list;
    loop->free_list = c;
}

static void conn_buffer_release(ELConnection *c)
{
    if (c->in_buf) {
        metrics_buffer_returned(ring_buffer_get_capacity(c->in_buf));
        ring_buffer_free(c->in_buf);
        c->in_buf = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* epoll helpers                                                        */
/* ------------------------------------------------------------------ */
static int epoll_mod(EventLoop *loop, ELConnection *c, uint32_t events)
{
    struct epoll_event ev;
    ev.events   = events;
    ev.data.ptr = c;
    return epoll_ctl(loop->epoll_fd, EPOLL_CTL_MOD, c->fd, &ev);
}

static int epoll_set_rw(EventLoop *loop, ELConnection *c)
{
    return epoll_mod(loop, c, EPOLLIN | EPOLLOUT);
}

static int epoll_set_ro(EventLoop *loop, ELConnection *c)
{
    return epoll_mod(loop, c, EPOLLIN);
}

/* ------------------------------------------------------------------ */
/* Listener backpressure                                                */
/* ------------------------------------------------------------------ */

/*
 * Number of connections waiting in the kernel accept queue for a listening
 * socket. On Linux, TCP_INFO's tcpi_unacked reports the accept-queue depth for
 * a listener; this is the "backlog pressure" observed while read interest is
 * disabled. Returns -1 when the platform does not report it.
 */
static long listener_backlog_depth(int fd)
{
#if defined(TCP_INFO) && defined(IPPROTO_TCP)
    struct tcp_info info;
    socklen_t len = sizeof(info);
    if (getsockopt(fd, IPPROTO_TCP, TCP_INFO, &info, &len) == 0)
        return (long)info.tcpi_unacked;
#endif
    return -1;
}

static void listener_set_enabled(EventLoop *loop, int enabled)
{
    if (loop->listen_fd < 0 || loop->listen_enabled == enabled)
        return;

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events   = enabled ? EPOLLIN : 0;
    ev.data.ptr = &loop->listen_sentinel;
    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_MOD, loop->listen_fd, &ev) != 0) {
        perror("epoll_ctl listener");
        return;
    }
    loop->listen_enabled = enabled;
    if (!enabled) {
        if (loop->listener_disabled_at_ms < 0)
            loop->listener_disabled_at_ms = now_ms();
        metrics_listener_disabled();
        metrics_backlog_depth(listener_backlog_depth(loop->listen_fd));
    } else {
        if (loop->listener_disabled_at_ms >= 0) {
            metrics_listener_enabled(now_ms() - loop->listener_disabled_at_ms);
            loop->listener_disabled_at_ms = -1;
        }
    }
}

static void maybe_enable_listener(EventLoop *loop)
{
    if (loop->listen_enabled)
        return;

    /* Still disabled: sample how many clients are queued behind the
     * backpressure so a saturation run can see the backlog build. */
    metrics_backlog_depth(listener_backlog_depth(loop->listen_fd));

    if (metrics_active_connection_count() < loop->capacity) {
        listener_set_enabled(loop, 1);
    }
}

/* ------------------------------------------------------------------ */
/* Pipeline queue helpers                                               */
/* ------------------------------------------------------------------ */
static int pq_full(const ELConnection *c)
{
    return c->pq_count >= MAX_PIPELINE_DEPTH;
}

static void pq_push(ELConnection *c, const PendingResponse *r)
{
    c->pq[c->pq_tail] = *r;
    c->pq_tail        = (c->pq_tail + 1) % (MAX_PIPELINE_DEPTH + 1);
    c->pq_count++;
}

static PendingResponse *pq_head_ptr(ELConnection *c)
{
    return c->pq_count ? &c->pq[c->pq_head] : NULL;
}

static void pq_pop(ELConnection *c)
{
    c->pq_head        = (c->pq_head + 1) % (MAX_PIPELINE_DEPTH + 1);
    c->pq_count--;
    c->out_header_sent = 0;
    c->out_body_sent   = 0;
}

/* ------------------------------------------------------------------ */
/* drain_socket                                                         */
/*                                                                      */
/* Discard data still in the kernel receive buffer so close() sends a   */
/* FIN rather than an RST.  Called before close() whenever the server   */
/* decides to close without reading all available input.                */
/* ------------------------------------------------------------------ */
static void drain_socket(int fd)
{
    char buf[4096];
    for (int i = 0; i < 64; i++) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0)
            break;
    }
}

/* ------------------------------------------------------------------ */
/* conn_close                                                           */
/* ------------------------------------------------------------------ */
static void conn_close(EventLoop *loop, ELConnection *c, ELCloseReason reason)
{
    c->close_reason = reason;
    ELConnState prev_state = c->state;
    c->state = CONN_CLOSING;

    epoll_ctl(loop->epoll_fd, EPOLL_CTL_DEL, c->fd, NULL);
    if (reason != CLOSE_SHUTDOWN) {
        drain_socket(c->fd);
    }
    close(c->fd);
    metrics_active_connection_dec();

    switch (reason) {
    case CLOSE_DEADLINE:
        if (prev_state == CONN_WRITING) {
            metrics_write_timeout();
        } else if (c->request_count == 0) {
            metrics_header_timeout();
        } else {
            metrics_idle_timeout();
        }
        break;
    case CLOSE_WRITE_ERROR:
        break;
    default:
        break;
    }

    conn_buffer_release(c);
    conn_return(loop, c);
    metrics_el_connection_closed();

    /* A closed connection frees a global slot: re-enable this loop's
     * listener if it was disabled for backpressure. During shutdown the
     * listener is never re-enabled. */
    if (loop->running && *loop->running) {
        maybe_enable_listener(loop);
    }
}

/* ------------------------------------------------------------------ */
/* conn_open                                                            */
/* ------------------------------------------------------------------ */
static int conn_open(EventLoop *loop, int fd)
{
    ELConnection *c = conn_alloc(loop);
    if (!c)
        return -1;

    c->fd    = fd;
    c->state = CONN_READING_HEADERS;
    /* Input storage is allocated lazily on the first readable event so idle
     * connections do not pin a request buffer. */
    c->in_buf = NULL;

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    deadline_set(&c->deadline, HEADER_READ_TIMEOUT_SEC);

    struct epoll_event ev;
    ev.events   = EPOLLIN;
    ev.data.ptr = c;
    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        conn_return(loop, c);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* process_input                                                        */
/* ------------------------------------------------------------------ */
static int process_input(EventLoop *loop, ELConnection *c)
{
    int queued = 0;

    if (!c->in_buf)
        return 0;

    while (!ring_buffer_is_empty(c->in_buf) &&
           !pq_full(c) &&
           c->request_count < MAX_KEEPALIVE_REQUESTS)
    {
        PendingResponse pr;
        int req_ka = 0;

        int force_last = (c->request_count + 1 >= MAX_KEEPALIVE_REQUESTS);

        int ret = el_prepare_response(c->in_buf, force_last, &req_ka, &pr);
        if (ret == 1) {
            break; /* NEED_DATA */
        }
        if (ret != 0) {
            conn_close(loop, c, CLOSE_PROTOCOL_ERROR);
            return -1;
        }

        c->keep_alive = req_ka;
        pq_push(c, &pr);
        c->request_count++;
        queued++;
    }

    if (pq_full(c)) {
        metrics_el_pipeline_full();
    }
    return queued;
}

/* ------------------------------------------------------------------ */
/* reject_overload                                                      */
/*                                                                      */
/* Bounded overload policy: consume the pending request, answer with a  */
/* complete 503 when possible, then half-close and drain so the peer    */
/* observes a FIN.  Never silently reset a request because the cap was  */
/* reached.                                                             */
/* ------------------------------------------------------------------ */
static void reject_overload(int fd)
{
    drain_socket(fd);
    if (g_overload_len) {
        ssize_t n = send(fd, g_overload_response, g_overload_len, MSG_NOSIGNAL);
        if (n == (ssize_t)g_overload_len) {
            metrics_overload_response();
        } else {
            metrics_connection_reset();
        }
    } else {
        metrics_connection_reset();
    }
    shutdown(fd, SHUT_WR);
    drain_socket(fd);
    close(fd);
}

/* ------------------------------------------------------------------ */
/* el_accept                                                            */
/* ------------------------------------------------------------------ */
static void el_accept(EventLoop *loop)
{
    for (;;) {
        /* Listener backpressure: stop accepting while the process-wide table
         * is full. Pending connections wait in the kernel accept queue. */
        if (metrics_active_connection_count() >= loop->capacity) {
            listener_set_enabled(loop, 0);
            return;
        }

        int fd;
#ifdef SOCK_NONBLOCK
        fd = accept4(loop->listen_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
#else
        fd = accept(loop->listen_fd, NULL, NULL);
        if (fd >= 0) {
            int flags = fcntl(fd, F_GETFL, 0);
            if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
                close(fd);
                fd = -1;
                errno = EBADF;
            }
            if (fd >= 0) {
                int cloexec = fcntl(fd, F_GETFD, 0);
                if (cloexec >= 0)
                    fcntl(fd, F_SETFD, cloexec | FD_CLOEXEC);
            }
        }
#endif
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            if (errno == EINTR)
                continue;
            perror("accept");
            return;
        }

        if (!metrics_connection_admit(loop->capacity)) {
            metrics_admission_rejected_reason(ADMISSION_REJECT_CAPACITY);
            reject_overload(fd);
            listener_set_enabled(loop, 0);
            return;
        }

        if (conn_open(loop, fd) < 0) {
            metrics_active_connection_dec();
            metrics_admission_rejected_reason(ADMISSION_REJECT_TABLE_FULL);
            reject_overload(fd);
            continue;
        }

        metrics_connection_accepted();
        metrics_el_connection_opened();
        metrics_el_loop_accepted(loop->id);
        loop->accepted++;
    }
}

/* ------------------------------------------------------------------ */
/* el_writable                                                          */
/* ------------------------------------------------------------------ */
static void el_writable(EventLoop *loop, ELConnection *c)
{
    metrics_el_writable_event();

    for (;;) {
        if (c->pq_count == 0 && c->in_buf && !ring_buffer_is_empty(c->in_buf)) {
            if (process_input(loop, c) == -1)
                return; /* closed */
            if (c->fd < 0)
                return;
            if (c->pq_count > 0) {
                c->state = CONN_WRITING;
                deadline_set(&c->deadline, WRITE_TIMEOUT_SEC);
            }
        }

        if (c->pq_count == 0)
            break;

        PendingResponse *pr = pq_head_ptr(c);

        /* --- Send headers --- */
        size_t hdr_rem = pr->header_len - c->out_header_sent;
        while (hdr_rem > 0) {
            ssize_t n = send(c->fd, pr->header + c->out_header_sent,
                             hdr_rem, MSG_NOSIGNAL);
            if (n > 0) {
                c->out_header_sent += (size_t)n;
                hdr_rem            -= (size_t)n;
                if (hdr_rem > 0) {
                    metrics_el_partial_write();
                    return;
                }
            } else if (n == 0) {
                return;
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    metrics_el_eagain();
                    return;
                }
                if (errno == EINTR)
                    continue;
                conn_close(loop, c, CLOSE_WRITE_ERROR);
                return;
            }
        }

        /* --- Send body --- */
        if (!pr->is_head && pr->body_len > 0) {
            size_t body_rem = pr->body_len - c->out_body_sent;
            while (body_rem > 0) {
                ssize_t n = send(c->fd, pr->body + c->out_body_sent,
                                 body_rem, MSG_NOSIGNAL);
                if (n > 0) {
                    c->out_body_sent += (size_t)n;
                    body_rem         -= (size_t)n;
                    if (body_rem > 0) {
                        metrics_el_partial_write();
                        return;
                    }
                } else if (n == 0) {
                    return;
                } else {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        metrics_el_eagain();
                        return;
                    }
                    if (errno == EINTR)
                        continue;
                    conn_close(loop, c, CLOSE_WRITE_ERROR);
                    return;
                }
            }
        }

        /* --- Response fully sent --- */
        metrics_response(pr->status);
        int fc = pr->force_close;
        pq_pop(c);

        if (fc) {
            conn_close(loop, c, CLOSE_KEEPALIVE_LIMIT);
            return;
        }
    }

    if (!c->keep_alive) {
        conn_close(loop, c, CLOSE_CLIENT_EOF);
        return;
    }

    /* Idle keep-alive: release the request buffer so memory scales with active
     * requests rather than the connection-table size. The buffer is recreated
     * on the next readable event. */
    if (c->in_buf && ring_buffer_is_empty(c->in_buf)) {
        conn_buffer_release(c);
    }

    c->state = CONN_KEEP_ALIVE;
    deadline_set(&c->deadline, IDLE_TIMEOUT_SEC);
    epoll_set_ro(loop, c);
    metrics_el_output_drained();
}

/* ------------------------------------------------------------------ */
/* el_readable                                                          */
/* ------------------------------------------------------------------ */
static void el_readable(EventLoop *loop, ELConnection *c)
{
    metrics_el_readable_event();

    /* Lazily allocate input storage for this connection. */
    if (!c->in_buf) {
        c->in_buf = ring_buffer_create(INITIAL_RING_BUFFER_CAPACITY);
        if (!c->in_buf) {
            conn_close(loop, c, CLOSE_BUFFER_FULL);
            return;
        }
        metrics_buffer_leased(ring_buffer_get_capacity(c->in_buf));
    }

    if (ring_buffer_get_size(c->in_buf) >= MAX_INPUT_BUFFER_BYTES) {
        metrics_input_buffer_limit();
        conn_close(loop, c, CLOSE_BUFFER_FULL);
        return;
    }

    char buf[EL_RECV_BUFSIZE];
    for (;;) {
        ssize_t n = recv(c->fd, buf, sizeof(buf), 0);
        if (n > 0) {
            ring_buffer_write(c->in_buf, buf, (size_t)n);
            if (ring_buffer_get_size(c->in_buf) >= MAX_INPUT_BUFFER_BYTES) {
                metrics_input_buffer_limit();
                conn_close(loop, c, CLOSE_BUFFER_FULL);
                return;
            }
        } else if (n == 0) {
            if (c->pq_count == 0) {
                conn_close(loop, c, CLOSE_CLIENT_EOF);
            } else {
                c->keep_alive = 0;
            }
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                metrics_el_eagain();
                break;
            }
            if (errno == EINTR)
                continue;
            conn_close(loop, c, CLOSE_CLIENT_EOF);
            return;
        }
    }

    if (process_input(loop, c) == -1)
        return;

    if (c->fd < 0)
        return;

    if (c->pq_count > 0 && c->state != CONN_WRITING) {
        c->state = CONN_WRITING;
        deadline_set(&c->deadline, WRITE_TIMEOUT_SEC);
        epoll_set_rw(loop, c);
    }
}

/* ------------------------------------------------------------------ */
/* el_scan_deadlines                                                    */
/* ------------------------------------------------------------------ */
static void el_scan_deadlines(EventLoop *loop)
{
    for (size_t i = 0; i < loop->pool_size; i++) {
        ELConnection *c = &loop->pool[i];
        if (c->fd < 0 || c->state == CONN_CLOSING)
            continue;
        if (deadline_expired(&c->deadline)) {
            metrics_el_deadline_close();
            conn_close(loop, c, CLOSE_DEADLINE);
        }
    }
}

/* ------------------------------------------------------------------ */
/* event_loop_main                                                      */
/* ------------------------------------------------------------------ */
static void event_loop_main(EventLoop *loop)
{
    struct epoll_event events[EL_MAX_EVENTS];

    while (*loop->running) {
        int nev = epoll_wait(loop->epoll_fd, events, EL_MAX_EVENTS,
                             EL_DEADLINE_SCAN_MS);
        metrics_el_wakeup();
        metrics_el_loop_wakeup(loop->id);
        loop->wakeups++;

        if (nev < 0) {
            if (errno == EINTR)
                continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nev; i++) {
            struct epoll_event *ev = &events[i];

            if (ev->data.ptr == &loop->listen_sentinel) {
                if (ev->events & EPOLLIN)
                    el_accept(loop);
                continue;
            }

            if (ev->data.ptr == &loop->wake_sentinel) {
                uint64_t wake_count;
                ssize_t n = read(loop->wake_fd, &wake_count, sizeof(wake_count));
                (void)n;
                continue;
            }

            ELConnection *c = (ELConnection *)ev->data.ptr;
            if (c->fd < 0)
                continue;

            if (ev->events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                conn_close(loop, c, CLOSE_CLIENT_EOF);
                continue;
            }

            if (ev->events & EPOLLOUT) {
                el_writable(loop, c);
            }

            if (c->fd >= 0 && (ev->events & EPOLLIN)) {
                el_readable(loop, c);
            }
        }

        el_scan_deadlines(loop);

        /* Another loop may have freed global capacity. */
        maybe_enable_listener(loop);
    }

    for (size_t i = 0; i < loop->pool_size; i++) {
        if (loop->pool[i].fd >= 0) {
            conn_close(loop, &loop->pool[i], CLOSE_SHUTDOWN);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Setup helpers                                                        */
/* ------------------------------------------------------------------ */
static int el_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return -1;
    return 0;
}

/*
 * Create an additional listening socket for a loop. SO_REUSEPORT lets several
 * loops bind the same address so the kernel hashes new connections across
 * them, avoiding a userspace acceptor and its fd handoff.
 */
static int create_reuseport_listener(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket reuseport");
        return -1;
    }

    int fl = fcntl(fd, F_GETFD);
    if (fl >= 0)
        fcntl(fd, F_SETFD, fl | FD_CLOEXEC);

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind reuseport");
        close(fd);
        return -1;
    }
    if (listen(fd, BACKLOG) < 0) {
        perror("listen reuseport");
        close(fd);
        return -1;
    }
    return fd;
}

static int loop_init(EventLoop *loop, int listen_fd, long capacity,
                     volatile sig_atomic_t *running)
{
    loop->listen_fd              = listen_fd;
    loop->listen_enabled         = 0;
    loop->capacity               = capacity;
    loop->running                = running;
    loop->listener_disabled_at_ms = -1;
    loop->epoll_fd               = -1;
    loop->wake_fd                = -1;

    if (el_set_nonblocking(listen_fd) < 0) {
        perror("fcntl listener nonblocking");
        return -1;
    }

    loop->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (loop->epoll_fd < 0) {
        perror("epoll_create1");
        return -1;
    }

    loop->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (loop->wake_fd < 0) {
        perror("eventfd");
        return -1;
    }

    if (pool_init(loop, (size_t)capacity) < 0) {
        perror("calloc connection table");
        return -1;
    }

    memset(&loop->listen_sentinel, 0, sizeof(loop->listen_sentinel));
    loop->listen_sentinel.fd = listen_fd;
    memset(&loop->wake_sentinel, 0, sizeof(loop->wake_sentinel));
    loop->wake_sentinel.fd = loop->wake_fd;

    struct epoll_event ev;
    ev.events   = EPOLLIN;
    ev.data.ptr = &loop->listen_sentinel;
    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev) < 0) {
        perror("epoll_ctl listener");
        return -1;
    }
    loop->listen_enabled = 1;

    ev.events   = EPOLLIN;
    ev.data.ptr = &loop->wake_sentinel;
    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, loop->wake_fd, &ev) < 0) {
        perror("epoll_ctl wake_fd");
        return -1;
    }
    return 0;
}

static void loop_destroy(EventLoop *loop, int close_listen)
{
    if (loop->wake_fd >= 0)
        close(loop->wake_fd);
    if (loop->epoll_fd >= 0)
        close(loop->epoll_fd);
    if (close_listen && loop->listen_fd >= 0)
        close(loop->listen_fd);
    free(loop->pool);
    loop->pool = NULL;
}

static void *loop_thread(void *arg)
{
    event_loop_main((EventLoop *)arg);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Resolve the requested loop count: explicit override or online CPU cores. */
int event_loop_thread_count(void)
{
    if (EL_THREAD_COUNT > 0)
        return EL_THREAD_COUNT;

    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (cores < 1)
        cores = 1;
    if (cores > EL_MAX_THREADS)
        cores = EL_MAX_THREADS;
    return (int)cores;
}

/* event_loop_run                                                       */
/* ------------------------------------------------------------------ */
int event_loop_run(int server_fd, volatile sig_atomic_t *running, long capacity,
                   int nloops)
{
    if (capacity <= 0) {
        fprintf(stderr, "event_loop_run: invalid capacity %ld\n", capacity);
        return -1;
    }

    build_overload_response();

    if (nloops < 1)
        nloops = 1;
    metrics_el_loop_count(nloops);

    EventLoop *loops = calloc((size_t)nloops, sizeof(EventLoop));
    if (!loops) {
        perror("calloc event loops");
        return -1;
    }

    pthread_t *threads = calloc((size_t)nloops, sizeof(pthread_t));
    if (!threads) {
        free(loops);
        return -1;
    }
    int started_threads = 0;

    int status = 0;
    for (int i = 0; i < nloops; i++) {
        int lfd = (i == 0) ? server_fd : create_reuseport_listener();
        if (lfd < 0) {
            status = -1;
            nloops = i; /* only initialise loops created so far */
            break;
        }
        if (loop_init(&loops[i], lfd, capacity, running) < 0) {
            if (i != 0)
                close(lfd);
            status = -1;
            nloops = i;
            break;
        }
        loops[i].id = i;
    }

    if (status == 0) {
        for (int i = 1; i < nloops; i++) {
            if (pthread_create(&threads[i], NULL, loop_thread, &loops[i]) != 0) {
                perror("pthread_create event loop");
                status = -1;
                nloops = i;
                break;
            }
            started_threads++;
        }
    }

    if (status == 0) {
        event_loop_main(&loops[0]);

        /* Wake the other loops so they observe the shutdown flag promptly,
         * then join them. */
        for (int i = 1; i < nloops; i++) {
            uint64_t one = 1;
            ssize_t n = write(loops[i].wake_fd, &one, sizeof(one));
            (void)n;
        }
        for (int i = 1; i <= started_threads; i++) {
            pthread_join(threads[i], NULL);
        }
    }

    /* Per-loop accept distribution: the kernel owns SO_REUSEPORT hashing, so
     * this is the only in-process evidence of how connections were spread. */
    for (int i = 0; i < nloops; i++) {
        fprintf(stderr, "event-loop %d: wakeups=%llu accepted=%llu\n",
                loops[i].id,
                (unsigned long long)loops[i].wakeups,
                (unsigned long long)loops[i].accepted);
    }

    for (int i = 0; i < nloops; i++) {
        loop_destroy(&loops[i], i != 0);
    }
    free(threads);
    free(loops);
    return status;
}
