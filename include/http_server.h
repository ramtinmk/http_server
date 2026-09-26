#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <stddef.h>

#include "ring_buffer.h"
#include "server_config.h"

/* Shared listening parameters (socket setup and startup diagnostics). */
#define PORT 8081
#ifndef BACKLOG
#define BACKLOG 1024
#endif

/*
 * Response descriptor for the event-loop path.
 *
 * Pointers are into startup-cached or static-string (literal) memory; the
 * event loop must NEVER free them. body may be NULL when body_len == 0.
 */
typedef struct {
    const char          *header;       /* Full header block to send.         */
    size_t               header_len;   /* Length of header block in bytes.   */
    const unsigned char *body;         /* Body bytes (may be NULL).          */
    size_t               body_len;     /* Length of body in bytes.           */
    int                  is_head;      /* HEAD request: skip body send.      */
    int                  force_close;  /* Close connection after this resp.  */
    int                  status;       /* HTTP status code (for metrics).    */
} PendingResponse;

/*
 * Parse one HTTP request from `rb` and populate `pr` with the response
 * descriptor that the event loop can enqueue and send without blocking.
 *
 * On return:
 *   0  — one request fully parsed; `pr` is filled; `*keep_alive_out` is set.
 *   1  — headers are incomplete; caller must buffer more data and retry.
 *         `rb` is left unchanged (transaction rolled back).
 */
int el_prepare_response(RingBuffer *rb, int force_close, int *keep_alive_out, PendingResponse *pr);

int create_server_socket(int reuseport);
int initialize_static_responses(void);

#endif /* HTTP_SERVER_H */
