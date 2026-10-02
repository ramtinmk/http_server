#ifndef SANDBOX_H
#define SANDBOX_H

#include "config.h"

/* Apply the independently configurable filesystem and syscall sandboxes.
 * Must run after listeners are bound and before any worker thread is started. */
int sandbox_apply(const ServerConfig *cfg, const char *metrics_path);

#endif
