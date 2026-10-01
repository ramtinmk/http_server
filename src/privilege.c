#define _GNU_SOURCE   /* setresuid, setresgid, PR_SET_NO_NEW_PRIVS */
/*
 * Phase 4: privilege drop and process hardening.
 *
 * See include/privilege.h. The drop is deliberately irreversible: all three
 * (real/effective/saved) uids and gids are set, so the process cannot regain
 * the identity it started with. It is applied only after every listener has
 * been bound, which lets an operator bind a low port or use
 * CAP_NET_BIND_SERVICE and then run the request path as an unprivileged user.
 */
#include "privilege.h"
#include "server_config.h"
#include "log.h"

#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/types.h>

typedef struct {
    int   have_uid;
    int   have_gid;
    uid_t uid;
    gid_t gid;
    char  name[RUN_USER_MAX];   /* resolved login name, or "" for numeric id */
    int   name_known;           /* 1 when `name` is usable for initgroups() */
} RunIdentity;

/* Parse a non-negative base-10 id. Returns 0 on success, -1 otherwise. */
static int parse_numeric(const char *s, long *out)
{
    errno = 0;
    char *end = NULL;
    long value = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || value < 0)
        return -1;
    *out = value;
    return 0;
}

/*
 * Resolve the configured identity. A named user supplies its primary gid when
 * run_group is unset; run_group overrides it. Fails, naming the key, when a
 * value is neither a known name nor a numeric id.
 */
static int resolve_identity(const ServerConfig *cfg, RunIdentity *id)
{
    memset(id, 0, sizeof(*id));

    if (cfg->run_group[0] != '\0') {
        long numeric = 0;
        struct group *gr = getgrnam(cfg->run_group);
        if (gr != NULL) {
            id->gid = gr->gr_gid;
            id->have_gid = 1;
        } else if (parse_numeric(cfg->run_group, &numeric) == 0) {
            id->gid = (gid_t)numeric;
            id->have_gid = 1;
        } else {
            fprintf(stderr, "FATAL: run_group=%s does not name a group and is "
                            "not a numeric gid\n", cfg->run_group);
            return -1;
        }
    }

    if (cfg->run_user[0] != '\0') {
        long numeric = 0;
        struct passwd *pw = getpwnam(cfg->run_user);
        if (pw != NULL) {
            id->uid = pw->pw_uid;
            id->have_uid = 1;
            snprintf(id->name, sizeof(id->name), "%s", pw->pw_name);
            id->name_known = 1;
            if (!id->have_gid) {
                id->gid = pw->pw_gid;
                id->have_gid = 1;
            }
        } else if (parse_numeric(cfg->run_user, &numeric) == 0) {
            id->uid = (uid_t)numeric;
            id->have_uid = 1;
        } else {
            fprintf(stderr, "FATAL: run_user=%s does not name a user and is "
                            "not a numeric uid\n", cfg->run_user);
            return -1;
        }
    }

    return 0;
}

int privilege_validate(const ServerConfig *cfg)
{
    if (cfg->run_user[0] == '\0' && cfg->run_group[0] == '\0')
        return 0;

    RunIdentity id;
    return resolve_identity(cfg, &id);
}

/*
 * Always-on hardening applied even when no identity is configured. Diminishing
 * the process prevents a core dump from exposing memory and forbids any future
 * privilege gain through a setuid helper.
 */
static int apply_process_hardening(void)
{
#ifdef PR_SET_DUMPABLE
    if (prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0) {
        fprintf(stderr, "FATAL: prctl(PR_SET_DUMPABLE, 0) failed: %s\n",
                strerror(errno));
        return -1;
    }
#endif
#ifdef PR_SET_NO_NEW_PRIVS
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        fprintf(stderr, "FATAL: prctl(PR_SET_NO_NEW_PRIVS) failed: %s\n",
                strerror(errno));
        return -1;
    }
#endif
    return 0;
}

int privilege_drop(const ServerConfig *cfg)
{
    RunIdentity id;
    if (resolve_identity(cfg, &id) != 0)
        return -1;

    if (id.have_uid || id.have_gid) {
        uid_t euid = geteuid();

        /* Without privilege the only permitted drop is a no-op (the target
         * equals the current identity), which still exercises the path. */
        if (euid != 0) {
            if ((id.have_uid && id.uid != getuid()) ||
                (id.have_gid && id.gid != getgid())) {
                fprintf(stderr,
                        "FATAL: run_user/run_group requires starting as root "
                        "(euid=%ld, target uid=%ld gid=%ld)\n",
                        (long)euid,
                        id.have_uid ? (long)id.uid : -1L,
                        id.have_gid ? (long)id.gid : -1L);
                return -1;
            }
        }

        /* Supplementary groups must be set before the gid changes and need
         * privilege to do so. */
        if (euid == 0) {
            if (id.name_known) {
                if (initgroups(id.name, id.have_gid ? id.gid : getgid()) != 0) {
                    fprintf(stderr, "FATAL: initgroups(%s) failed: %s\n",
                            id.name, strerror(errno));
                    return -1;
                }
            } else if (setgroups(0, NULL) != 0) {
                fprintf(stderr, "FATAL: setgroups(0) failed: %s\n",
                        strerror(errno));
                return -1;
            }
        }

        if (id.have_gid && setresgid(id.gid, id.gid, id.gid) != 0) {
            fprintf(stderr, "FATAL: setresgid(%ld) failed: %s\n",
                    (long)id.gid, strerror(errno));
            return -1;
        }
        if (id.have_uid && setresuid(id.uid, id.uid, id.uid) != 0) {
            fprintf(stderr, "FATAL: setresuid(%ld) failed: %s\n",
                    (long)id.uid, strerror(errno));
            return -1;
        }

        if ((id.have_uid && getuid() != id.uid) ||
            (id.have_gid && getgid() != id.gid)) {
            fprintf(stderr,
                    "FATAL: privilege drop failed (uid=%ld gid=%ld, wanted "
                    "uid=%ld gid=%ld)\n",
                    (long)getuid(), (long)getgid(),
                    id.have_uid ? (long)id.uid : -1L,
                    id.have_gid ? (long)id.gid : -1L);
            return -1;
        }

        char uid_str[32] = "unchanged";
        char gid_str[32] = "unchanged";
        if (id.have_uid)
            snprintf(uid_str, sizeof(uid_str), "%ld", (long)id.uid);
        if (id.have_gid)
            snprintf(gid_str, sizeof(gid_str), "%ld", (long)id.gid);
        printf("Privilege drop: uid=%s gid=%s, groups cleared, "
               "no_new_privs=1\n", uid_str, gid_str);
    }

    if (apply_process_hardening() != 0)
        return -1;

    log_msg(LOG_LEVEL_INFO,
            "privilege_drop uid=%ld gid=%ld run_user=%s run_group=%s",
            (long)getuid(), (long)getgid(),
            cfg->run_user[0] ? cfg->run_user : "(none)",
            cfg->run_group[0] ? cfg->run_group : "(none)");
    return 0;
}
