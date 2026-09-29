#ifndef SD_NOTIFY_H
#define SD_NOTIFY_H

/*
 * Minimal systemd readiness protocol (sd_notify) without linking libsystemd.
 *
 * Sends a datagram to $NOTIFY_SOCKET when it is set and returns 0; a no-op
 * returning 0 when the variable is unset. A send failure returns -1 and is
 * non-fatal: the server keeps running with a warning when it is not launched by
 * systemd or the socket is unreachable.
 */

int sd_notify_ready(const char *status);
int sd_notify_stopping(const char *status);

#endif /* SD_NOTIFY_H */
