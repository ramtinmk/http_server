#ifndef PRIVILEGE_H
#define PRIVILEGE_H

/*
 * Phase 4: privilege drop and always-on process hardening.
 *
 * `run_user` / `run_group` accept a POSIX name or a numeric id. When both are
 * empty the server keeps the invoking identity and only applies the always-on
 * hardening (PR_SET_NO_NEW_PRIVS, non-dumpable).
 */

#include "config.h"

/*
 * Resolve cfg->run_user/run_group at startup. Returns 0 when unset or
 * resolvable, -1 when a value does not name a known user/group and is not a
 * numeric id (the message names the offending key). Called before any listener
 * is bound so a typo fails fast.
 */
int privilege_validate(const ServerConfig *cfg);

/*
 * Permanently drop to the configured uid/gid (all three ids), clear
 * supplementary groups, and set PR_SET_NO_NEW_PRIVS and PR_SET_DUMPABLE=0.
 *
 * Must be called after every listener (plaintext + TLS, all loops) is bound so
 * a privileged port no longer needs the elevated id, and before any event-loop
 * thread starts. A configured target that differs from the current identity
 * requires starting as root; otherwise the call fails closed. Returns 0 on
 * success, -1 on any failure (fatal).
 */
int privilege_drop(const ServerConfig *cfg);

#endif /* PRIVILEGE_H */
