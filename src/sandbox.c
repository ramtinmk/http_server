#define _GNU_SOURCE
#include "sandbox.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/types.h>

#if defined(__has_include)
#if __has_include(<linux/landlock.h>)
#include <linux/landlock.h>
#define HAVE_LANDLOCK_HEADERS 1
#endif
#endif
#ifndef HAVE_LANDLOCK_HEADERS
#define LANDLOCK_CREATE_RULESET_VERSION 0x1
#define LANDLOCK_RULE_PATH_BENEATH 1
#define LANDLOCK_ACCESS_FS_EXECUTE (1ULL << 0)
#define LANDLOCK_ACCESS_FS_WRITE_FILE (1ULL << 1)
#define LANDLOCK_ACCESS_FS_READ_FILE (1ULL << 2)
#define LANDLOCK_ACCESS_FS_READ_DIR (1ULL << 3)
#define LANDLOCK_ACCESS_FS_REMOVE_DIR (1ULL << 4)
#define LANDLOCK_ACCESS_FS_REMOVE_FILE (1ULL << 5)
#define LANDLOCK_ACCESS_FS_MAKE_CHAR (1ULL << 6)
#define LANDLOCK_ACCESS_FS_MAKE_DIR (1ULL << 7)
#define LANDLOCK_ACCESS_FS_MAKE_REG (1ULL << 8)
#define LANDLOCK_ACCESS_FS_MAKE_SOCK (1ULL << 9)
#define LANDLOCK_ACCESS_FS_MAKE_FIFO (1ULL << 10)
#define LANDLOCK_ACCESS_FS_MAKE_BLOCK (1ULL << 11)
#define LANDLOCK_ACCESS_FS_MAKE_SYM (1ULL << 12)
#define LANDLOCK_ACCESS_FS_REFER (1ULL << 13)
#define LANDLOCK_ACCESS_FS_TRUNCATE (1ULL << 14)
struct landlock_ruleset_attr {
    uint64_t handled_access_fs;
};
struct landlock_path_beneath_attr {
    uint64_t allowed_access;
    int parent_fd;
};
#endif
#ifndef SYS_landlock_create_ruleset
#define SYS_landlock_create_ruleset 444
#define SYS_landlock_add_rule 445
#define SYS_landlock_restrict_self 446
#endif
#endif

#if defined(__linux__)
static int landlock_add_path(int ruleset_fd, const char *path,
                             uint64_t allowed_access)
{
    if (!path || !*path)
        return 0;
    char resolved[PATH_MAX];
    if (!realpath(path, resolved)) {
        fprintf(stderr, "FATAL: Landlock cannot resolve path '%s': %s\n",
                path, strerror(errno));
        return -1;
    }
    int path_fd = open(resolved, O_PATH | O_CLOEXEC);
    if (path_fd < 0) {
        fprintf(stderr, "FATAL: Landlock cannot open path '%s': %s\n",
                resolved, strerror(errno));
        return -1;
    }
    struct landlock_path_beneath_attr rule;
    memset(&rule, 0, sizeof(rule));
    rule.allowed_access = allowed_access;
    rule.parent_fd = path_fd;
    int rc = (int)syscall(SYS_landlock_add_rule, ruleset_fd,
                          LANDLOCK_RULE_PATH_BENEATH, &rule, 0);
    int saved_errno = errno;
    close(path_fd);
    if (rc != 0) {
        fprintf(stderr, "FATAL: Landlock rule for '%s' failed: %s\n",
                resolved, strerror(saved_errno));
        return -1;
    }
    return 0;
}

static int landlock_add_parent(int ruleset_fd, const char *path,
                               uint64_t allowed_access)
{
    if (!path || !*path)
        return 0;
    char parent[PATH_MAX];
    size_t n = strlen(path);
    if (n >= sizeof(parent)) {
        errno = ENAMETOOLONG;
        fprintf(stderr, "FATAL: Landlock path is too long: '%s'\n", path);
        return -1;
    }
    memcpy(parent, path, n + 1);
    char *slash = strrchr(parent, '/');
    if (slash) {
        if (slash == parent)
            slash[1] = '\0';
        else
            *slash = '\0';
    } else {
        snprintf(parent, sizeof(parent), ".");
    }
    char resolved[PATH_MAX];
    if (!realpath(parent, resolved)) {
        fprintf(stderr, "FATAL: Landlock cannot resolve path '%s': %s\n",
                parent, strerror(errno));
        return -1;
    }
    return landlock_add_path(ruleset_fd, resolved, allowed_access);
}

static int apply_landlock(const ServerConfig *cfg, const char *metrics_path)
{
    int abi = (int)syscall(SYS_landlock_create_ruleset, NULL, 0,
                           LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 1) {
        fprintf(stderr, "FATAL: Landlock requested but unavailable: %s\n",
                strerror(errno));
        return -1;
    }

    uint64_t handled = LANDLOCK_ACCESS_FS_EXECUTE |
                       LANDLOCK_ACCESS_FS_WRITE_FILE |
                       LANDLOCK_ACCESS_FS_READ_FILE |
                       LANDLOCK_ACCESS_FS_READ_DIR |
                       LANDLOCK_ACCESS_FS_REMOVE_DIR |
                       LANDLOCK_ACCESS_FS_REMOVE_FILE |
                       LANDLOCK_ACCESS_FS_MAKE_CHAR |
                       LANDLOCK_ACCESS_FS_MAKE_DIR |
                       LANDLOCK_ACCESS_FS_MAKE_REG |
                       LANDLOCK_ACCESS_FS_MAKE_SOCK |
                       LANDLOCK_ACCESS_FS_MAKE_FIFO |
                       LANDLOCK_ACCESS_FS_MAKE_BLOCK |
                       LANDLOCK_ACCESS_FS_MAKE_SYM;
#ifdef LANDLOCK_ACCESS_FS_REFER
    if (abi >= 2)
        handled |= LANDLOCK_ACCESS_FS_REFER;
#endif
#ifdef LANDLOCK_ACCESS_FS_TRUNCATE
    if (abi >= 3)
        handled |= LANDLOCK_ACCESS_FS_TRUNCATE;
#endif

    struct landlock_ruleset_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.handled_access_fs = handled;
    int ruleset_fd = (int)syscall(SYS_landlock_create_ruleset, &attr,
                                  sizeof(attr), 0);
    if (ruleset_fd < 0) {
        fprintf(stderr, "FATAL: Landlock ruleset creation failed: %s\n",
                strerror(errno));
        return -1;
    }

    uint64_t traverse = LANDLOCK_ACCESS_FS_EXECUTE;
    uint64_t read_only = traverse | LANDLOCK_ACCESS_FS_READ_FILE |
                         LANDLOCK_ACCESS_FS_READ_DIR;
    uint64_t log_access = traverse | LANDLOCK_ACCESS_FS_WRITE_FILE;
#ifdef LANDLOCK_ACCESS_FS_TRUNCATE
    if (abi >= 3)
        log_access |= LANDLOCK_ACCESS_FS_TRUNCATE;
#endif
    uint64_t metrics_access = read_only | LANDLOCK_ACCESS_FS_WRITE_FILE |
                              LANDLOCK_ACCESS_FS_REMOVE_FILE |
                              LANDLOCK_ACCESS_FS_MAKE_REG;
#ifdef LANDLOCK_ACCESS_FS_REFER
    if (abi >= 2)
        metrics_access |= LANDLOCK_ACCESS_FS_REFER;
#endif

    int failed = 0;
    /* Execute-only traversal at the top, then explicitly grant data access. */
    failed |= landlock_add_path(ruleset_fd, "/", traverse) != 0;
    failed |= landlock_add_path(ruleset_fd, cfg->document_root, read_only) != 0;
    failed |= landlock_add_path(ruleset_fd, "/proc/self", read_only) != 0;
    failed |= landlock_add_parent(ruleset_fd, cfg->tls_cert_file, read_only) != 0;
    failed |= landlock_add_parent(ruleset_fd, cfg->tls_key_file, read_only) != 0;
    failed |= landlock_add_parent(ruleset_fd, cfg->log_file, log_access) != 0;
    failed |= landlock_add_parent(ruleset_fd, cfg->config_path, read_only) != 0;
    if (metrics_path && *metrics_path)
        failed |= landlock_add_parent(ruleset_fd, metrics_path, metrics_access) != 0;

    if (!failed && prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        fprintf(stderr, "FATAL: Landlock requires no_new_privs: %s\n",
                strerror(errno));
        failed = 1;
    }
    if (!failed && syscall(SYS_landlock_restrict_self, ruleset_fd, 0) != 0) {
        fprintf(stderr, "FATAL: Landlock restrict_self failed: %s\n",
                strerror(errno));
        failed = 1;
    }
    if (!failed && strcmp(cfg->document_root, "/") != 0) {
        int probe = open("/etc/passwd", O_RDONLY | O_CLOEXEC);
        if (probe >= 0) {
            close(probe);
            fprintf(stderr, "FATAL: Landlock denied-path self-test unexpectedly succeeded\n");
            failed = 1;
        } else if (errno != EACCES && errno != EPERM) {
            fprintf(stderr, "FATAL: Landlock denied-path self-test failed: %s\n",
                    strerror(errno));
            failed = 1;
        }
    }
    close(ruleset_fd);
    return failed ? -1 : 0;
}

static int apply_seccomp(void)
{
#if defined(__x86_64__)
    const uint32_t expected_arch = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
    const uint32_t expected_arch = AUDIT_ARCH_AARCH64;
#else
    fprintf(stderr, "FATAL: seccomp allowlist has no syscall ABI for this CPU\n");
    return -1;
#endif

    static const int allowed_syscalls[] = {
#define SYS_ALLOW(name) __NR_##name,
#ifdef __NR_read
        SYS_ALLOW(read)
#endif
#ifdef __NR_write
        SYS_ALLOW(write)
#endif
#ifdef __NR_readv
        SYS_ALLOW(readv)
#endif
#ifdef __NR_writev
        SYS_ALLOW(writev)
#endif
#ifdef __NR_close
        SYS_ALLOW(close)
#endif
#ifdef __NR_fstat
        SYS_ALLOW(fstat)
#endif
#ifdef __NR_newfstatat
        SYS_ALLOW(newfstatat)
#endif
#ifdef __NR_lseek
        SYS_ALLOW(lseek)
#endif
#ifdef __NR_pread64
        SYS_ALLOW(pread64)
#endif
#ifdef __NR_openat
        SYS_ALLOW(openat)
#endif
#ifdef __NR_openat2
        SYS_ALLOW(openat2)
#endif
#ifdef __NR_readlink
        SYS_ALLOW(readlink)
#endif
#ifdef __NR_readlinkat
        SYS_ALLOW(readlinkat)
#endif
#ifdef __NR_getdents64
        SYS_ALLOW(getdents64)
#endif
#ifdef __NR_mmap
        SYS_ALLOW(mmap)
#endif
#ifdef __NR_mprotect
        SYS_ALLOW(mprotect)
#endif
#ifdef __NR_munmap
        SYS_ALLOW(munmap)
#endif
#ifdef __NR_brk
        SYS_ALLOW(brk)
#endif
#ifdef __NR_madvise
        SYS_ALLOW(madvise)
#endif
#ifdef __NR_mremap
        SYS_ALLOW(mremap)
#endif
#ifdef __NR_rt_sigaction
        SYS_ALLOW(rt_sigaction)
#endif
#ifdef __NR_rt_sigprocmask
        SYS_ALLOW(rt_sigprocmask)
#endif
#ifdef __NR_rt_sigreturn
        SYS_ALLOW(rt_sigreturn)
#endif
#ifdef __NR_sigaltstack
        SYS_ALLOW(sigaltstack)
#endif
#ifdef __NR_ioctl
        SYS_ALLOW(ioctl)
#endif
#ifdef __NR_fcntl
        SYS_ALLOW(fcntl)
#endif
#ifdef __NR_dup
        SYS_ALLOW(dup)
#endif
#ifdef __NR_dup2
        SYS_ALLOW(dup2)
#endif
#ifdef __NR_dup3
        SYS_ALLOW(dup3)
#endif
#ifdef __NR_pipe2
        SYS_ALLOW(pipe2)
#endif
#ifdef __NR_poll
        SYS_ALLOW(poll)
#endif
#ifdef __NR_ppoll
        SYS_ALLOW(ppoll)
#endif
#ifdef __NR_epoll_create1
        SYS_ALLOW(epoll_create1)
#endif
#ifdef __NR_epoll_ctl
        SYS_ALLOW(epoll_ctl)
#endif
#ifdef __NR_epoll_wait
        SYS_ALLOW(epoll_wait)
#endif
#ifdef __NR_epoll_pwait
        SYS_ALLOW(epoll_pwait)
#endif
#ifdef __NR_eventfd2
        SYS_ALLOW(eventfd2)
#endif
#ifdef __NR_accept4
        SYS_ALLOW(accept4)
#endif
#ifdef __NR_recvfrom
        SYS_ALLOW(recvfrom)
#endif
#ifdef __NR_sendto
        SYS_ALLOW(sendto)
#endif
#ifdef __NR_sendmsg
        SYS_ALLOW(sendmsg)
#endif
#ifdef __NR_recvmsg
        SYS_ALLOW(recvmsg)
#endif
#ifdef __NR_socket
        SYS_ALLOW(socket)
#endif
#ifdef __NR_bind
        SYS_ALLOW(bind)
#endif
#ifdef __NR_listen
        SYS_ALLOW(listen)
#endif
#ifdef __NR_getsockname
        SYS_ALLOW(getsockname)
#endif
#ifdef __NR_getpeername
        SYS_ALLOW(getpeername)
#endif
#ifdef __NR_getsockopt
        SYS_ALLOW(getsockopt)
#endif
#ifdef __NR_setsockopt
        SYS_ALLOW(setsockopt)
#endif
#ifdef __NR_shutdown
        SYS_ALLOW(shutdown)
#endif
#ifdef __NR_sendfile
        SYS_ALLOW(sendfile)
#endif
#ifdef __NR_clock_gettime
        SYS_ALLOW(clock_gettime)
#endif
#ifdef __NR_clock_nanosleep
        SYS_ALLOW(clock_nanosleep)
#endif
#ifdef __NR_nanosleep
        SYS_ALLOW(nanosleep)
#endif
#ifdef __NR_futex
        SYS_ALLOW(futex)
#endif
#ifdef __NR_set_robust_list
        SYS_ALLOW(set_robust_list)
#endif
#ifdef __NR_rseq
        SYS_ALLOW(rseq)
#endif
#ifdef __NR_gettid
        SYS_ALLOW(gettid)
#endif
#ifdef __NR_getpid
        SYS_ALLOW(getpid)
#endif
#ifdef __NR_getuid
        SYS_ALLOW(getuid)
#endif
#ifdef __NR_getgid
        SYS_ALLOW(getgid)
#endif
#ifdef __NR_geteuid
        SYS_ALLOW(geteuid)
#endif
#ifdef __NR_getegid
        SYS_ALLOW(getegid)
#endif
#ifdef __NR_prctl
        SYS_ALLOW(prctl)
#endif
#ifdef __NR_prlimit64
        SYS_ALLOW(prlimit64)
#endif
#ifdef __NR_getrlimit
        SYS_ALLOW(getrlimit)
#endif
#ifdef __NR_sched_getaffinity
        SYS_ALLOW(sched_getaffinity)
#endif
#ifdef __NR_sched_yield
        SYS_ALLOW(sched_yield)
#endif
#ifdef __NR_getrandom
        SYS_ALLOW(getrandom)
#endif
#ifdef __NR_clone
        SYS_ALLOW(clone)
#endif
#ifdef __NR_clone3
        SYS_ALLOW(clone3)
#endif
#ifdef __NR_tgkill
        SYS_ALLOW(tgkill)
#endif
#ifdef __NR_exit
        SYS_ALLOW(exit)
#endif
#ifdef __NR_exit_group
        SYS_ALLOW(exit_group)
#endif
#ifdef __NR_restart_syscall
        SYS_ALLOW(restart_syscall)
#endif
#ifdef __NR_unlink
        SYS_ALLOW(unlink)
#endif
#ifdef __NR_unlinkat
        SYS_ALLOW(unlinkat)
#endif
#ifdef __NR_rename
        SYS_ALLOW(rename)
#endif
#ifdef __NR_renameat
        SYS_ALLOW(renameat)
#endif
#ifdef __NR_renameat2
        SYS_ALLOW(renameat2)
#endif
#ifdef __NR_mkdir
        SYS_ALLOW(mkdir)
#endif
#ifdef __NR_mkdirat
        SYS_ALLOW(mkdirat)
#endif
#ifdef __NR_uname
        SYS_ALLOW(uname)
#endif
#ifdef __NR_sysinfo
        SYS_ALLOW(sysinfo)
#endif
#ifdef __NR_fsync
        SYS_ALLOW(fsync)
#endif
#ifdef __NR_fdatasync
        SYS_ALLOW(fdatasync)
#endif
#ifdef __NR_gettimeofday
        SYS_ALLOW(gettimeofday)
#endif
#ifdef __NR_membarrier
        SYS_ALLOW(membarrier)
#endif
#undef SYS_ALLOW
    };

    struct sock_filter filter[5 + 2 * (sizeof(allowed_syscalls) /
                                       sizeof(allowed_syscalls[0])) + 1];
    size_t count = 0;
    filter[count++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                                                   offsetof(struct seccomp_data, arch));
    filter[count++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                                                   expected_arch, 1, 0);
    filter[count++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K,
                                                   SECCOMP_RET_KILL_PROCESS);
    filter[count++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                                                   offsetof(struct seccomp_data, nr));
    for (size_t i = 0; i < sizeof(allowed_syscalls) / sizeof(allowed_syscalls[0]); i++) {
        filter[count++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                                                       (uint32_t)allowed_syscalls[i], 0, 1);
        filter[count++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K,
                                                       SECCOMP_RET_ALLOW);
    }
    filter[count++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K,
                                                   SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA));
    struct sock_fprog program = {
        .len = (unsigned short)count,
        .filter = filter
    };
    if (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER,
                SECCOMP_FILTER_FLAG_TSYNC, &program) != 0) {
        fprintf(stderr, "FATAL: seccomp syscall allowlist failed: %s\n",
                strerror(errno));
        return -1;
    }
    return 0;
}
#endif

int sandbox_apply(const ServerConfig *cfg, const char *metrics_path)
{
    if (!cfg->landlock_enabled && !cfg->seccomp_enabled)
        return 0;
#if !defined(__linux__)
    (void)metrics_path;
    fprintf(stderr, "FATAL: requested OS sandbox is only available on Linux\n");
    return -1;
#else
    if (cfg->landlock_enabled && apply_landlock(cfg, metrics_path) != 0)
        return -1;
    if (cfg->seccomp_enabled && apply_seccomp() != 0)
        return -1;
    fprintf(stderr, "OS sandbox active: landlock=%d seccomp=%d\n",
            cfg->landlock_enabled, cfg->seccomp_enabled);
    return 0;
#endif
}
