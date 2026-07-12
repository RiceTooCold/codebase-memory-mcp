/*
 * compat_fs.c — Portable file system operations.
 *
 * POSIX: direct wrappers around opendir/readdir/closedir, popen/pclose, mkdir, unlink.
 * Windows: FindFirstFile/FindNextFile, _popen/_pclose, _mkdir, _unlink.
 */
#include "foundation/constants.h"
#include "foundation/compat_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

/* ── Windows implementation ────────────────────────────────── */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <direct.h> /* _wmkdir */
#include <io.h>     /* _wunlink */
#include "foundation/win_utf8.h"

struct cbm_dir {
    HANDLE find_handle;
    WIN32_FIND_DATAW find_data;
    wchar_t wide_pattern[CBM_PATH_MAX];
    cbm_dirent_t entry;
    bool first;
    bool done;
};

cbm_dir_t *cbm_opendir(const char *path) {
    if (!path) {
        return NULL;
    }
    wchar_t *wpath = cbm_utf8_to_wide(path);
    if (!wpath) {
        return NULL;
    }

    size_t wlen = wcslen(wpath);
    if (wlen == 0 || wlen + 2 >= CBM_PATH_MAX) {
        free(wpath);
        return NULL;
    }

    cbm_dir_t *d = (cbm_dir_t *)calloc(CBM_ALLOC_ONE, sizeof(cbm_dir_t));
    if (!d) {
        free(wpath);
        return NULL;
    }

    wmemcpy(d->wide_pattern, wpath, wlen + 1);
    wchar_t *p = d->wide_pattern + wlen - SKIP_ONE;
    if (*p != L'\\' && *p != L'/') {
        ++p;
        *p++ = L'\\';
    } else {
        ++p;
    }
    *p++ = L'*';
    *p = L'\0';
    free(wpath);

    d->find_handle = FindFirstFileW(d->wide_pattern, &d->find_data);
    if (d->find_handle == INVALID_HANDLE_VALUE) {
        free(d);
        return NULL;
    }
    d->first = true;
    d->done = false;
    return d;
}

cbm_dirent_t *cbm_readdir(cbm_dir_t *d) {
    if (!d || d->done) {
        return NULL;
    }
    if (!d->first) {
        if (!FindNextFileW(d->find_handle, &d->find_data)) {
            d->done = true;
            return NULL;
        }
    }
    d->first = false;

    while (d->find_data.cFileName[0] == L'.' &&
           (d->find_data.cFileName[1] == L'\0' ||
            (d->find_data.cFileName[1] == L'.' && d->find_data.cFileName[2] == L'\0'))) {
        if (!FindNextFileW(d->find_handle, &d->find_data)) {
            d->done = true;
            return NULL;
        }
    }

    char *u8 = cbm_wide_to_utf8(d->find_data.cFileName);
    if (!u8) {
        d->done = true;
        return NULL;
    }
    size_t nlen = strlen(u8);
    if (nlen >= CBM_DIRENT_NAME_MAX) {
        nlen = CBM_DIRENT_NAME_MAX - SKIP_ONE;
    }
    memcpy(d->entry.name, u8, nlen);
    d->entry.name[nlen] = '\0';
    free(u8);
    d->entry.is_dir = (d->find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    d->entry.d_type = 0;
    return &d->entry;
}

void cbm_closedir(cbm_dir_t *d) {
    if (d) {
        if (d->find_handle != INVALID_HANDLE_VALUE) {
            FindClose(d->find_handle);
        }
        free(d);
    }
}

FILE *cbm_popen(const char *cmd, const char *mode) {
    return _popen(cmd, mode);
}

int cbm_pclose(FILE *f) {
    return _pclose(f);
}

/* Windows: no deadline enforcement — _popen exposes no process handle to
 * kill, and the callers' git commands are already bounded by --max-count.
 * Proper CreateProcess-based timeouts are a documented follow-up. */
bool cbm_proc_open(cbm_proc_t *p, const char *cmd, int timeout_ms) {
    (void)timeout_ms;
    p->timed_out = false;
    p->fp = _popen(cmd, "r");
    return p->fp != NULL;
}

bool cbm_proc_gets(cbm_proc_t *p, char *out, size_t cap) {
    return fgets(out, (int)cap, p->fp) != NULL;
}

int cbm_proc_close(cbm_proc_t *p) {
    return _pclose(p->fp);
}

bool cbm_mkdir_p(const char *path, int mode) {
    (void)mode;
    wchar_t *wpath = cbm_utf8_to_wide(path);
    if (!wpath) {
        return false;
    }

    if (_wmkdir(wpath) == 0) {
        free(wpath);
        return true;
    }
    size_t wlen = wcslen(wpath);
    wchar_t *tmp = (wchar_t *)malloc((wlen + 1) * sizeof(wchar_t));
    if (!tmp) {
        free(wpath);
        return false;
    }
    wmemcpy(tmp, wpath, wlen + 1);
    for (wchar_t *p = tmp + SKIP_ONE; *p; p++) {
        if (*p == L'/' || *p == L'\\') {
            *p = L'\0';
            _wmkdir(tmp);
            *p = L'\\';
        }
    }
    bool ok = _wmkdir(tmp) == 0 || GetLastError() == ERROR_ALREADY_EXISTS;
    free(tmp);
    free(wpath);
    return ok;
}

int cbm_unlink(const char *path) {
    wchar_t *wpath = cbm_utf8_to_wide(path);
    if (!wpath) {
        return CBM_NOT_FOUND;
    }
    int ret = _wunlink(wpath);
    free(wpath);
    return ret;
}

int cbm_rmdir(const char *path) {
    wchar_t *wpath = cbm_utf8_to_wide(path);
    if (!wpath) {
        return CBM_NOT_FOUND;
    }
    int ret = _wrmdir(wpath);
    free(wpath);
    return ret;
}

int cbm_exec_no_shell(const char *const *argv) {
    if (!argv || !argv[0]) {
        return CBM_NOT_FOUND;
    }
    return (int)_spawnvp(_P_WAIT, argv[0], argv);
}

#else /* POSIX */

/* ── POSIX implementation ────────────────────────────────── */

#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct cbm_dir {
    DIR *dir;
    cbm_dirent_t entry;
};

cbm_dir_t *cbm_opendir(const char *path) {
    if (!path) {
        return NULL;
    }
    DIR *dir = opendir(path);
    if (!dir) {
        return NULL;
    }
    cbm_dir_t *d = (cbm_dir_t *)calloc(CBM_ALLOC_ONE, sizeof(cbm_dir_t));
    if (!d) {
        closedir(dir);
        return NULL;
    }
    d->dir = dir;
    return d;
}

cbm_dirent_t *cbm_readdir(cbm_dir_t *d) {
    if (!d || !d->dir) {
        return NULL;
    }
    struct dirent *de;
    while ((de = readdir(d->dir)) != NULL) {
        /* Skip "." and ".." */
        if (de->d_name[0] == '.' &&
            (de->d_name[SKIP_ONE] == '\0' ||
             (de->d_name[SKIP_ONE] == '.' && de->d_name[PAIR_LEN] == '\0'))) {
            continue;
        }
        size_t nlen = strlen(de->d_name);
        if (nlen >= CBM_DIRENT_NAME_MAX) {
            nlen = CBM_DIRENT_NAME_MAX - SKIP_ONE;
        }
        memcpy(d->entry.name, de->d_name, nlen);
        d->entry.name[nlen] = '\0';
        d->entry.is_dir = (de->d_type == DT_DIR);
        d->entry.d_type = de->d_type;
        return &d->entry;
    }
    return NULL;
}

void cbm_closedir(cbm_dir_t *d) {
    if (d) {
        if (d->dir) {
            closedir(d->dir);
        }
        free(d);
    }
}

FILE *cbm_popen(const char *cmd, const char *mode) {
    return popen(cmd, mode);
}

int cbm_pclose(FILE *f) {
    return pclose(f);
}

enum { CBM_MS_PER_SEC = 1000, CBM_NS_PER_MS = 1000000, CBM_PROC_EXEC_FAIL = 127 };

static long long proc_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * CBM_MS_PER_SEC + ts.tv_nsec / CBM_NS_PER_MS;
}

bool cbm_proc_open(cbm_proc_t *p, const char *cmd, int timeout_ms) {
    memset(p, 0, sizeof(*p));
    p->fd = -1;

    int fds[2];
    if (pipe(fds) != 0) {
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    if (pid == 0) {
        /* Own process group so a timeout can kill the shell AND whatever
         * it spawned (git), not just the shell. */
        setpgid(0, 0);
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(CBM_PROC_EXEC_FAIL);
    }
    close(fds[1]);
    setpgid(pid, pid); /* mirror the child's call so a kill can never race it */
    p->fd = fds[0];
    p->pid = (int)pid;
    p->deadline_ms = timeout_ms > 0 ? proc_now_ms() + timeout_ms : 0;
    return true;
}

/* Emit up to cap-1 buffered bytes ending at the first newline (or the
 * whole buffer when force is set / it is cap-filling). fgets semantics. */
static bool proc_emit(cbm_proc_t *p, char *out, size_t cap, bool force) {
    size_t take = 0;
    while (take < p->buf_len && p->buf[take] != '\n') {
        take++;
    }
    if (take < p->buf_len) {
        take++; /* include the newline */
    } else if (!force && p->buf_len < cap - SKIP_ONE) {
        return false; /* no full line buffered yet */
    }
    if (take == 0) {
        return false;
    }
    if (take > cap - SKIP_ONE) {
        take = cap - SKIP_ONE;
    }
    memcpy(out, p->buf, take);
    out[take] = '\0';
    p->buf_len -= take;
    memmove(p->buf, p->buf + take, p->buf_len);
    return true;
}

bool cbm_proc_gets(cbm_proc_t *p, char *out, size_t cap) {
    if (cap < PAIR_LEN || p->timed_out) {
        return false;
    }
    for (;;) {
        if (proc_emit(p, out, cap, p->eof || p->buf_len == sizeof(p->buf))) {
            return true;
        }
        if (p->eof) {
            return false;
        }
        int wait_ms = -1;
        if (p->deadline_ms > 0) {
            long long remaining = p->deadline_ms - proc_now_ms();
            if (remaining <= 0) {
                p->timed_out = true;
                return false;
            }
            wait_ms = (int)remaining;
        }
        struct pollfd pfd = {.fd = p->fd, .events = POLLIN};
        int prc = poll(&pfd, 1, wait_ms);
        if (prc == 0) {
            p->timed_out = true;
            return false;
        }
        if (prc < 0) {
            if (errno == EINTR) {
                continue;
            }
            p->eof = true;
            continue;
        }
        ssize_t got = read(p->fd, p->buf + p->buf_len, sizeof(p->buf) - p->buf_len);
        if (got > 0) {
            p->buf_len += (size_t)got;
        } else if (got == 0 || errno != EINTR) {
            p->eof = true;
        }
    }
}

int cbm_proc_close(cbm_proc_t *p) {
    if (p->fd >= 0) {
        close(p->fd);
        p->fd = -1;
    }
    if (p->pid <= 0) {
        return -1;
    }
    if (p->timed_out && kill(-(pid_t)p->pid, SIGKILL) != 0) {
        kill((pid_t)p->pid, SIGKILL);
    }
    int status = 0;
    pid_t rc;
    do {
        rc = waitpid((pid_t)p->pid, &status, 0);
    } while (rc < 0 && errno == EINTR);
    p->pid = 0;
    if (p->timed_out || rc < 0 || !WIFEXITED(status)) {
        return -1;
    }
    return WEXITSTATUS(status);
}

bool cbm_mkdir_p(const char *path, int mode) {
    /* Try direct mkdir first */
    if (mkdir(path, (mode_t)mode) == 0) {
        return true;
    }
    /* Walk path and create each component */
    char *tmp = strdup(path);
    if (!tmp) {
        return false;
    }
    for (char *p = tmp + SKIP_ONE; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, (mode_t)mode); /* ignore intermediate errors */
            *p = '/';
        }
    }
    bool ok = (mkdir(tmp, (mode_t)mode) == 0 || errno == EEXIST) != 0;
    free(tmp);
    return ok;
}

int cbm_unlink(const char *path) {
    return unlink(path);
}

int cbm_rmdir(const char *path) {
    return rmdir(path);
}

int cbm_exec_no_shell(const char *const *argv) {
    if (!argv || !argv[0]) {
        return CBM_NOT_FOUND;
    }
    pid_t pid = fork();
    if (pid < 0) {
        return CBM_NOT_FOUND;
    }
    if (pid == 0) {
        /* Child: exec directly — no shell interpretation */
        /* 127 = standard "command not found" exit code (POSIX convention) */
        enum { EXEC_NOT_FOUND = 127 };
        execvp(argv[0], (char *const *)argv);
        _exit(EXEC_NOT_FOUND);
    }
    /* Parent: wait for child */
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return CBM_NOT_FOUND;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return CBM_NOT_FOUND; /* killed by signal */
}

#endif /* _WIN32 */
