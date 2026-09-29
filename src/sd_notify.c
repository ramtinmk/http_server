#define _GNU_SOURCE   /* SOCK_CLOEXEC */

#include "sd_notify.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/*
 * Build "KEY=1\nSTATUS=...\n" into buf. `state` is "READY" or "STOPPING".
 * STATUS is omitted when `status` is NULL or empty. Returns the length.
 */
static size_t build_message(char *buf, size_t cap, const char *state,
                            const char *status)
{
    int n = snprintf(buf, cap, "%s=1\n", state);
    if (n < 0)
        return 0;
    size_t off = (size_t)n;
    if (status && *status && off < cap) {
        n = snprintf(buf + off, cap - off, "STATUS=%s\n", status);
        if (n > 0)
            off += (size_t)n;
    }
    if (off >= cap)
        off = cap - 1;
    return off;
}

static int notify(const char *state, const char *status)
{
    const char *path = getenv("NOTIFY_SOCKET");
    if (!path || !*path)
        return 0;   /* not launched by systemd: nothing to notify */

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    size_t plen = strlen(path);
    socklen_t alen;
    if (path[0] == '@') {
        /* Abstract namespace: leading NUL instead of '@'. */
        if (plen > sizeof(addr.sun_path))
            return -1;
        memcpy(addr.sun_path, path, plen);
        addr.sun_path[0] = '\0';
        alen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + plen);
    } else {
        if (plen >= sizeof(addr.sun_path))
            return -1;
        memcpy(addr.sun_path, path, plen + 1);
        alen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + plen + 1);
    }

    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;

    char message[512];
    size_t len = build_message(message, sizeof(message), state, status);

    ssize_t sent = sendto(fd, message, len, 0,
                          (struct sockaddr *)&addr, alen);
    close(fd);
    return sent < 0 ? -1 : 0;
}

int sd_notify_ready(const char *status)
{
    return notify("READY", status);
}

int sd_notify_stopping(const char *status)
{
    return notify("STOPPING", status);
}
