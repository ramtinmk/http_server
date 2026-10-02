#ifndef CLIENT_LIMITS_H
#define CLIENT_LIMITS_H

#include "config.h"
#include <sys/socket.h>

/* Initialize the process-wide, bounded per-IP accounting table. */
int client_limits_init(const ServerConfig *cfg);

/* Reserve/release one active connection for a numeric peer-address key. */
int client_limits_connection_admit(const char *peer);
void client_limits_connection_release(const char *peer);

/* Consume one request-rate token; returns 1 when allowed, 0 when limited. */
int client_limits_request_admit(const char *peer);

#endif
