#define _GNU_SOURCE

#include "event_loop.h"
#include "metrics.h"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Local constants                                                      */
/* ------------------------------------------------------------------ */
#define EL_RECV_BUFSIZE 8192

/* ------------------------------------------------------------------ */
/* Connection pool                                                      */
/* ------------------------------------------------------------------ */
static ELConnection  g_conn_pool[MAX_ACTIVE_CONNECTIONS];
static ELConnection *g_free_list = NULL;

/* Sentinel used to identify the listening socket in epoll data.ptr. */
static ELConnection  g_listen_sentinel;

/* ------------------------------------------------------------------ */
/* Deadline helpers                                                     */
/* ------------------------------------------------------------------ */
static void now_mono(struct timespec *ts)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
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
/* Pool management                                                      */
/* ------------------------------------------------------------------ */
static void pool_init(void)
{
    g_free_list = NULL;
    for (int i = MAX_ACTIVE_CONNECTIONS - 1; i >= 0; i--) {
        g_conn_pool[i].fd   = -1;
        g_conn_pool[i].next = g_free_list;
        g_free_list         = &g_conn_pool[i];
    }
}

static ELConnection *conn_alloc(void)
{
    if (!g_free_list)
        return NULL;
    ELConnection *c = g_free_list;
    g_free_list     = c->next;
    memset(c, 0, sizeof(*c));
    c->fd   = -1;
    c->next = NULL;
    return c;
}

static void conn_return(ELConnection *c)
{
    c->fd   = -1;
    c->next = g_free_list;
    g_free_list = c;
}

/* ------------------------------------------------------------------ */
/* epoll helpers                                                        */
/* ------------------------------------------------------------------ */
static int epoll_mod(int efd, ELConnection *c, uint32_t events)
{
    struct epoll_event ev;
    ev.events   = events;
    ev.data.ptr = c;
    return epoll_ctl(efd, EPOLL_CTL_MOD, c->fd, &ev);
}

static int epoll_set_rw(int efd, ELConnection *c)
{
    return epoll_mod(efd, c, EPOLLIN | EPOLLOUT);
}

static int epoll_set_ro(int efd, ELConnection *c)
{
    return epoll_mod(efd, c, EPOLLIN);
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
/* Discard any data still in the kernel receive buffer so that close()  */
/* sends a FIN rather than a RST.  Called before close() whenever the  */
/* server decides to close without reading all available input.         */
/* ------------------------------------------------------------------ */
static void drain_socket(int fd)
{
    char buf[4096];
    /* Nonblocking: loop until EAGAIN or error (cap iterations for safety). */
    for (int i = 0; i < 64; i++) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
    }
}

/* ------------------------------------------------------------------ */
/* conn_close                                                           */
/* ------------------------------------------------------------------ */
static void conn_close(int epoll_fd, ELConnection *c, ELCloseReason reason)
{
    c->close_reason = reason;
    /* Capture state BEFORE overwriting it. */
    ELConnState prev_state = c->state;
    c->state = CONN_CLOSING;

    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, c->fd, NULL);
    /* Drain unread kernel receive-buffer data so the peer sees FIN not RST. */
    if (reason != CLOSE_SHUTDOWN) {
        drain_socket(c->fd);
    }
    close(c->fd);
    metrics_active_connection_dec();

    /* Reason-specific metrics */
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
        /* No extra metric; EPIPE etc. already counted in writable event. */
        break;
    default:
        break;
    }

    ring_buffer_free(c->in_buf);
    c->in_buf = NULL;
    conn_return(c);
    metrics_el_connection_closed();
}

/* ------------------------------------------------------------------ */
/* conn_open                                                            */
/* ------------------------------------------------------------------ */
static int conn_open(int epoll_fd, int fd)
{
    ELConnection *c = conn_alloc();
    if (!c)
        return -1;

    c->fd    = fd;
    c->state = CONN_READING_HEADERS;
    c->in_buf = ring_buffer_create(INITIAL_RING_BUFFER_CAPACITY);
    if (!c->in_buf) {
        conn_return(c);
        return -1;
    }

    /* Disable Nagle: cached responses are emitted as a header send followed
     * by a body send, and the second small write must not wait for an ACK of
     * the first.  A slow/sleeping client otherwise stalls each response on
     * the 40 ms delayed-ACK timer. */
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    deadline_set(&c->deadline, HEADER_READ_TIMEOUT_SEC);

    struct epoll_event ev;
    ev.events   = EPOLLIN;
    ev.data.ptr = c;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        ring_buffer_free(c->in_buf);
        c->in_buf = NULL;
        conn_return(c);
        return -1;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* process_input                                                        */
/* ------------------------------------------------------------------ */
static int process_input(int epoll_fd, ELConnection *c)
{
    int queued = 0;

    while (!ring_buffer_is_empty(c->in_buf) &&
           !pq_full(c) &&
           c->request_count < MAX_KEEPALIVE_REQUESTS)
    {
        PendingResponse pr;
        int req_ka = 0;

        /* The response to the request that would exceed the keep-alive limit
         * is the last one on this connection; it must advertise close. */
        int force_last = (c->request_count + 1 >= MAX_KEEPALIVE_REQUESTS);

        int ret = el_prepare_response(c->in_buf, force_last, &req_ka, &pr);
        if (ret == 1) {
            /* NEED_DATA */
            break;
        }
        if (ret != 0) {
            /* Protocol error */
            conn_close(epoll_fd, c, CLOSE_PROTOCOL_ERROR);
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
/* el_accept                                                            */
/* ------------------------------------------------------------------ */
static void el_accept(int epoll_fd, int server_fd)
{
    for (;;) {
        int fd;
#ifdef SOCK_NONBLOCK
        fd = accept4(server_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
#else
        fd = accept(server_fd, NULL, NULL);
        if (fd >= 0) {
            int flags = fcntl(fd, F_GETFL, 0);
            if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
                close(fd);
                fd = -1;
                errno = EBADF;
            }
            if (fd >= 0) {
                int cloexec_flags = fcntl(fd, F_GETFD, 0);
                if (cloexec_flags >= 0)
                    fcntl(fd, F_SETFD, cloexec_flags | FD_CLOEXEC);
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

        /* Admission check */
        if (metrics_active_connection_count() >= MAX_ACTIVE_CONNECTIONS) {
            metrics_admission_rejected();
            close(fd);
            continue;
        }

        if (conn_open(epoll_fd, fd) < 0) {
            metrics_admission_rejected();
            close(fd);
            continue;
        }

        metrics_active_connection_inc();
        metrics_connection_accepted();
        metrics_el_connection_opened();
    }
}

/* ------------------------------------------------------------------ */
/* el_writable                                                          */
/* ------------------------------------------------------------------ */
static void el_writable(int epoll_fd, ELConnection *c)
{
    metrics_el_writable_event();

    for (;;) {
        /* Refill the pipeline from buffered input.  A single recv() pass
         * only queues MAX_PIPELINE_DEPTH responses; when that cap pauses
         * parsing, more requests may be sitting unparsed in the ring
         * buffer even though no further EPOLLIN will arrive.  Parse them
         * now that queue space exists. */
        if (c->pq_count == 0 && !ring_buffer_is_empty(c->in_buf)) {
            if (process_input(epoll_fd, c) == -1) return; /* closed */
            if (c->fd < 0) return;
            if (c->pq_count > 0) {
                c->state = CONN_WRITING;
                deadline_set(&c->deadline, WRITE_TIMEOUT_SEC);
            }
        }

        if (c->pq_count == 0) break;

        PendingResponse *pr = pq_head_ptr(c);

        /* --- Send headers --- */
        size_t hdr_rem  = pr->header_len - c->out_header_sent;

        while (hdr_rem > 0) {
            ssize_t n = send(c->fd,
                             pr->header + c->out_header_sent,
                             hdr_rem,
                             MSG_NOSIGNAL);
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
                conn_close(epoll_fd, c, CLOSE_WRITE_ERROR);
                return;
            }
        }

        /* --- Send body --- */
        if (!pr->is_head && pr->body_len > 0) {
            size_t body_rem = pr->body_len - c->out_body_sent;

            while (body_rem > 0) {
                ssize_t n = send(c->fd,
                                 pr->body + c->out_body_sent,
                                 body_rem,
                                 MSG_NOSIGNAL);
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
                    conn_close(epoll_fd, c, CLOSE_WRITE_ERROR);
                    return;
                }
            }
        }

        /* --- Response fully sent --- */
        metrics_response(pr->status);
        int fc = pr->force_close;
        pq_pop(c);

        if (fc) {
            conn_close(epoll_fd, c, CLOSE_KEEPALIVE_LIMIT);
            return;
        }
    }

    /* All pending output drained */
    if (!c->keep_alive) {
        conn_close(epoll_fd, c, CLOSE_CLIENT_EOF);
        return;
    }

    c->state = CONN_KEEP_ALIVE;
    deadline_set(&c->deadline, IDLE_TIMEOUT_SEC);
    epoll_set_ro(epoll_fd, c);
    metrics_el_output_drained();
}

/* ------------------------------------------------------------------ */
/* el_readable                                                          */
/* ------------------------------------------------------------------ */
static void el_readable(int epoll_fd, ELConnection *c)
{
    metrics_el_readable_event();

    /* Buffer-full check before recv */
    if (ring_buffer_get_size(c->in_buf) >= MAX_INPUT_BUFFER_BYTES) {
        metrics_input_buffer_limit();
        conn_close(epoll_fd, c, CLOSE_BUFFER_FULL);
        return;
    }

    /* Drain recv into ring buffer */
    char buf[EL_RECV_BUFSIZE];
    for (;;) {
        ssize_t n = recv(c->fd, buf, sizeof(buf), 0);
        if (n > 0) {
            ring_buffer_write(c->in_buf, buf, (size_t)n);

            /* Re-check buffer limit after writing */
            if (ring_buffer_get_size(c->in_buf) >= MAX_INPUT_BUFFER_BYTES) {
                metrics_input_buffer_limit();
                conn_close(epoll_fd, c, CLOSE_BUFFER_FULL);
                return;
            }
        } else if (n == 0) {
            /* Clean EOF */
            if (c->pq_count == 0) {
                conn_close(epoll_fd, c, CLOSE_CLIENT_EOF);
            } else {
                c->keep_alive = 0;
                /* Let output drain; writable handler will close after. */
            }
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                metrics_el_eagain();
                break;
            }
            if (errno == EINTR)
                continue;
            conn_close(epoll_fd, c, CLOSE_CLIENT_EOF);
            return;
        }
    }

    /* Process whatever is in the buffer */
    if (process_input(epoll_fd, c) == -1)
        return; /* Connection was closed by process_input */

    /* Verify connection is still valid before touching it */
    if (c->fd < 0)
        return;

    /* Transition to CONN_WRITING if responses are queued */
    if (c->pq_count > 0 && c->state != CONN_WRITING) {
        c->state = CONN_WRITING;
        deadline_set(&c->deadline, WRITE_TIMEOUT_SEC);
        epoll_set_rw(epoll_fd, c);
    }
}

/* ------------------------------------------------------------------ */
/* el_scan_deadlines                                                    */
/* ------------------------------------------------------------------ */
static void el_scan_deadlines(int epoll_fd)
{
    for (int i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
        ELConnection *c = &g_conn_pool[i];
        if (c->fd < 0 || c->state == CONN_CLOSING)
            continue;
        if (deadline_expired(&c->deadline)) {
            metrics_el_deadline_close();
            conn_close(epoll_fd, c, CLOSE_DEADLINE);
        }
    }
}

/* ------------------------------------------------------------------ */
/* event_loop_run                                                       */
/* ------------------------------------------------------------------ */
int event_loop_run(int server_fd, volatile sig_atomic_t *running)
{
    /* Make the listening socket nonblocking */
    {
        int flags = fcntl(server_fd, F_GETFL, 0);
        if (flags < 0 || fcntl(server_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            perror("fcntl server_fd nonblocking");
            return -1;
        }
    }

    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
        perror("epoll_create1");
        return -1;
    }

    /* Register listening socket via sentinel */
    memset(&g_listen_sentinel, 0, sizeof(g_listen_sentinel));
    g_listen_sentinel.fd = server_fd;

    {
        struct epoll_event ev;
        ev.events   = EPOLLIN;
        ev.data.ptr = &g_listen_sentinel;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) < 0) {
            perror("epoll_ctl server_fd");
            close(epoll_fd);
            return -1;
        }
    }

    pool_init();

    struct epoll_event events[EL_MAX_EVENTS];

    while (*running) {
        int nev = epoll_wait(epoll_fd, events, EL_MAX_EVENTS, EL_DEADLINE_SCAN_MS);
        metrics_el_wakeup();

        if (nev < 0) {
            if (errno == EINTR)
                continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nev; i++) {
            struct epoll_event *ev = &events[i];

            if (ev->data.ptr == &g_listen_sentinel) {
                el_accept(epoll_fd, server_fd);
                continue;
            }

            ELConnection *c = (ELConnection *)ev->data.ptr;

            /* Stale event (connection already closed) */
            if (c->fd < 0)
                continue;

            /* Error conditions */
            if (ev->events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                conn_close(epoll_fd, c, CLOSE_CLIENT_EOF);
                continue;
            }

            /* Drain output first to minimise latency */
            if (ev->events & EPOLLOUT) {
                el_writable(epoll_fd, c);
            }

            /* Read (check fd still valid after writable handler) */
            if (c->fd >= 0 && (ev->events & EPOLLIN)) {
                el_readable(epoll_fd, c);
            }
        }

        el_scan_deadlines(epoll_fd);
    }

    /* Clean shutdown: close all active connections */
    for (int i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
        if (g_conn_pool[i].fd >= 0) {
            conn_close(epoll_fd, &g_conn_pool[i], CLOSE_SHUTDOWN);
        }
    }

    close(epoll_fd);
    return 0;
}
