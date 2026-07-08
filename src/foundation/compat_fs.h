/*
 * compat_fs.h — Portable directory iteration, popen, and file operations.
 *
 * POSIX: thin wrappers around opendir/readdir, popen/pclose, mkdir, unlink.
 * Windows: FindFirstFile/FindNextFile, _popen/_pclose, _mkdir, _unlink.
 */
#ifndef CBM_COMPAT_FS_H
#define CBM_COMPAT_FS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* ── Directory iteration ──────────────────────────────────────── */

/* Max filename length (MAX_PATH on Windows, NAME_MAX on POSIX). */
#define CBM_DIRENT_NAME_MAX 260

typedef struct cbm_dir cbm_dir_t;

typedef struct {
    char name[CBM_DIRENT_NAME_MAX];
    bool is_dir;
    unsigned char d_type; /* DT_REG, DT_DIR, DT_LNK, etc. (POSIX only, 0 on Windows) */
} cbm_dirent_t;

/* Open a directory for iteration. Returns NULL on error. */
cbm_dir_t *cbm_opendir(const char *path);

/* Read next entry. Returns NULL when done. The returned pointer is
 * valid until the next cbm_readdir call on the same handle. */
cbm_dirent_t *cbm_readdir(cbm_dir_t *d);

/* Close directory handle. */
void cbm_closedir(cbm_dir_t *d);

/* ── Portable popen/pclose ────────────────────────────────────── */

FILE *cbm_popen(const char *cmd, const char *mode);
int cbm_pclose(FILE *f);

/* ── Deadline-bounded command reader ──────────────────────────── */

/* popen("r")-like handle whose reads observe a wall-clock deadline, so a
 * runaway subprocess (e.g. an expensive git history walk) cannot hang the
 * caller. POSIX: pipe + fork of `sh -c cmd` in its own process group —
 * on expiry the group is killed and close() never blocks. Windows: plain
 * _popen with no deadline enforcement (documented follow-up); timed_out
 * stays false. */
enum { CBM_PROC_BUF = 4096 };

typedef struct {
#ifdef _WIN32
    FILE *fp;
#else
    int fd;
    int pid;
    long long deadline_ms; /* CLOCK_MONOTONIC, ms; 0 = no deadline */
    size_t buf_len;
    bool eof;
    char buf[CBM_PROC_BUF];
#endif
    bool timed_out;
} cbm_proc_t;

/* Start `sh -c cmd` with stdout piped to the handle. timeout_ms <= 0
 * means no deadline. Returns false when the process could not start. */
bool cbm_proc_open(cbm_proc_t *p, const char *cmd, int timeout_ms);

/* Read the next line (newline kept, fgets-style: at most cap-1 bytes).
 * Returns false on EOF or deadline expiry — check p->timed_out. */
bool cbm_proc_gets(cbm_proc_t *p, char *out, size_t cap);

/* Reap the child. Returns its exit code, or -1 when it was killed on
 * timeout, did not exit normally, or could not be reaped. */
int cbm_proc_close(cbm_proc_t *p);

/* ── File operations ──────────────────────────────────────────── */

/* Create directory (and parents). mode is ignored on Windows. Returns true on success. */
bool cbm_mkdir_p(const char *path, int mode);

/* Delete a file. Returns 0 on success. */
int cbm_unlink(const char *path);

/* Delete an empty directory. Returns 0 on success. */
int cbm_rmdir(const char *path);

/* Execute a command without shell interpretation.
 * argv is a NULL-terminated array: {"cmd", "arg1", "arg2", NULL}.
 * Returns the process exit code, or -1 on fork/exec failure.
 * POSIX: fork() + execvp(). Windows: _spawnvp(). */
int cbm_exec_no_shell(const char *const *argv);

#endif /* CBM_COMPAT_FS_H */
