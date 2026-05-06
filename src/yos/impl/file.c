/* impl/file.c — host FILE* table shim for the wasm guest.
 *
 * The FreeBSD wasm guest expects `FILE *` values to be opaque pointers
 * it can pass back to fread / fwrite / fclose / setbuf / etc. We don't
 * ship a guest libc with a FILE struct, so we synthesise small int
 * handles that index a host-side table of real glibc `FILE *`s and
 * present those as pointers to the guest. The guest treats them as
 * opaque addresses (it never dereferences them), so the trick works.
 *
 * Two reserved handles for stdin/stdout/stderr: nvim doesn't import
 * env.__stdinp / __stdoutp / __stderrp as data symbols (we checked) —
 * they're declared as extern data and resolved by wasm-ld to BSS.
 * Result: nvim's `stderr` reads as 0. We expose explicit fopen-of-
 * dev/stderr only if needed; for now the buffering family treats
 * NULL fp as stdout/stderr based on context where possible.
 *
 * Table layout: index 0 unused (so handle != 0 stays a valid "non
 * NULL" check in the guest). Indices 1..MAX-1 hold host FILE *.
 */

#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>

#include "yos/types.h"
#include "yos/ydebug.h"

#define YOS_FILE_MAX 256
static FILE *yos_files[YOS_FILE_MAX];
static pthread_mutex_t yos_file_lock = PTHREAD_MUTEX_INITIALIZER;

extern int yos_fd_get(struct yos_exec_ctx *ctx, int wasm_fd);

FILE *yos_handle_to_file(uint32_t h)
{
    /* Pre-bound stream handles: 1 = stdin, 2 = stdout, 3 = stderr. */
    if (h == 1) return stdin;
    if (h == 2) return stdout;
    if (h == 3) return stderr;
    if (h < 4 || h >= YOS_FILE_MAX) return NULL;
    return yos_files[h];
}
#define handle_to_file yos_handle_to_file

/* For the pre-bound stream handles (1/2/3), look up the host fd the
 * wasm guest's per-ctx fd_map currently maps to wasm fd 0/1/2.
 * Returns -1 if the handle isn't a stream handle.
 *
 * Why this matters: zsh redirects stdin/stdout/stderr via dup2 (e.g.
 * `dup2(pipe_w, 1)`) so that its `echo` builtin (which writes via
 * stdio's __stdoutp = (FILE *)2) lands on the pipe. yos's stdio
 * bridge naively maps __stdoutp to the host's *global* stdout, which
 * bypasses the dup2: the child writes "inner" to host stdout instead
 * of into the pipe, so `$(echo inner)` captures nothing. By routing
 * fp=1/2/3 through fd_map to a raw write/read on the current host
 * fd, the redirect actually takes effect. */
static int std_handle_hfd(struct yos_exec_ctx *ctx, uint32_t h)
{
    extern int yos_fd_get(struct yos_exec_ctx *, int);
    if (h == 1) return yos_fd_get(ctx, 0);
    if (h == 2) return yos_fd_get(ctx, 1);
    if (h == 3) return yos_fd_get(ctx, 2);
    return -1;
}

static uint32_t alloc_handle(FILE *f)
{
    if (!f) return 0;
    pthread_mutex_lock(&yos_file_lock);
    for (uint32_t i = 4; i < YOS_FILE_MAX; i++) {
        if (yos_files[i] == NULL) {
            yos_files[i] = f;
            pthread_mutex_unlock(&yos_file_lock);
            return i;
        }
    }
    pthread_mutex_unlock(&yos_file_lock);
    return 0;
}

static void free_handle(uint32_t h)
{
    if (h < 4 || h >= YOS_FILE_MAX) return;
    pthread_mutex_lock(&yos_file_lock);
    yos_files[h] = NULL;
    pthread_mutex_unlock(&yos_file_lock);
}

/* ── fopen / fclose / fdopen / freopen ─────────────────────────────── */

uint32_t yos_fopen(struct yos_exec_ctx *ctx, uint32_t path_off, uint32_t mode_off)
{
    if (!path_off || !mode_off) return 0;
    if (path_off >= ctx->memory_size || mode_off >= ctx->memory_size) return 0;
    const char *path = (const char *)(ctx->memory + path_off);
    const char *mode = (const char *)(ctx->memory + mode_off);
    FILE *f = fopen(path, mode);
    if (!f) return 0;
    uint32_t h = alloc_handle(f);
    if (!h) fclose(f);
    return h;
}

uint32_t yos_freopen(struct yos_exec_ctx *ctx, uint32_t path_off,
                    uint32_t mode_off, uint32_t fp)
{
    if (!path_off || !mode_off) return 0;
    FILE *f_old = handle_to_file(fp);
    const char *path = (const char *)(ctx->memory + path_off);
    const char *mode = (const char *)(ctx->memory + mode_off);
    FILE *f_new = freopen(path, mode, f_old ? f_old : NULL);
    if (!f_new) return 0;
    /* If reopened in-place, return the same handle. */
    if (f_new == f_old) return fp;
    if (fp >= 4 && fp < YOS_FILE_MAX) free_handle(fp);
    return alloc_handle(f_new);
}

uint32_t yos_fdopen(struct yos_exec_ctx *ctx, int32_t wfd, uint32_t mode_off)
{
    int hfd = yos_fd_get(ctx, wfd);
    if (hfd < 0) return 0;
    if (!mode_off || mode_off >= ctx->memory_size) return 0;
    const char *mode = (const char *)(ctx->memory + mode_off);
    FILE *f = fdopen(hfd, mode);
    if (!f) return 0;
    return alloc_handle(f);
}

int32_t yos_fclose(struct yos_exec_ctx *ctx, uint32_t fp)
{
    (void)ctx;
    FILE *f = handle_to_file(fp);
    if (!f) { errno = EBADF; return -1; }
    int r = fclose(f);
    if (fp >= 4 && fp < YOS_FILE_MAX) free_handle(fp);
    return r < 0 ? -errno : 0;
}

/* ── fread / fwrite ─────────────────────────────────────────────────── */

uint32_t yos_fread(struct yos_exec_ctx *ctx, uint32_t buf, uint32_t size,
                   uint32_t nmemb, uint32_t fp)
{
    FILE *f = handle_to_file(fp);
    if (!f || !size) return 0;
    if (buf + (uint64_t)size * nmemb > ctx->memory_size) return 0;
    return (uint32_t)fread(ctx->memory + buf, size, nmemb, f);
}

uint32_t yos_fwrite(struct yos_exec_ctx *ctx, uint32_t buf, uint32_t size,
                    uint32_t nmemb, uint32_t fp)
{
    if (!size) return 0;
    if (buf + (uint64_t)size * nmemb > ctx->memory_size) return 0;
    /* Route stream handles through the per-ctx host fd so dup2'd
     * stdin/stdout/stderr actually take effect. See std_handle_hfd. */
    int hfd = std_handle_hfd(ctx, fp);
    if (hfd >= 0) {
        size_t total = (size_t)size * nmemb;
        ssize_t w = write(hfd, ctx->memory + buf, total);
        if (w <= 0) return 0;
        return (uint32_t)((size_t)w / size);
    }
    FILE *f = handle_to_file(fp);
    if (!f) return 0;
    return (uint32_t)fwrite(ctx->memory + buf, size, nmemb, f);
}

uint32_t yos_fread_unlocked(struct yos_exec_ctx *ctx, uint32_t buf,
                            uint32_t size, uint32_t nmemb, uint32_t fp)
{
    return yos_fread(ctx, buf, size, nmemb, fp);
}

uint32_t yos_fwrite_unlocked(struct yos_exec_ctx *ctx, uint32_t buf,
                             uint32_t size, uint32_t nmemb, uint32_t fp)
{
    return yos_fwrite(ctx, buf, size, nmemb, fp);
}

/* ── single-char and string I/O ───────────────────────────────────── */

int32_t yos_fgetc(struct yos_exec_ctx *ctx, uint32_t fp) { (void)ctx; FILE *f=handle_to_file(fp); return f?fgetc(f):-1; }
int32_t yos_getc (struct yos_exec_ctx *ctx, uint32_t fp) { (void)ctx; FILE *f=handle_to_file(fp); return f?getc(f):-1; }
int32_t yos_getc_unlocked (struct yos_exec_ctx *ctx, uint32_t fp) { (void)ctx; FILE *f=handle_to_file(fp); return f?getc_unlocked(f):-1; }
int32_t yos_fputc(struct yos_exec_ctx *ctx, int32_t c, uint32_t fp) { (void)ctx; FILE *f=handle_to_file(fp); return f?fputc(c,f):-1; }
int32_t yos_putc (struct yos_exec_ctx *ctx, int32_t c, uint32_t fp) { (void)ctx; FILE *f=handle_to_file(fp); return f?putc(c,f):-1; }
int32_t yos_putc_unlocked (struct yos_exec_ctx *ctx, int32_t c, uint32_t fp) { (void)ctx; FILE *f=handle_to_file(fp); return f?putc_unlocked(c,f):-1; }
int32_t yos_ungetc(struct yos_exec_ctx *ctx, int32_t c, uint32_t fp) { (void)ctx; FILE *f=handle_to_file(fp); return f?ungetc(c,f):-1; }

uint32_t yos_fgets(struct yos_exec_ctx *ctx, uint32_t buf, int32_t n, uint32_t fp)
{
    FILE *f = handle_to_file(fp);
    if (!f || n <= 0 || buf + (uint32_t)n > ctx->memory_size) return 0;
    char *r = fgets((char *)(ctx->memory + buf), n, f);
    return r ? buf : 0;
}

int32_t yos_fputs(struct yos_exec_ctx *ctx, uint32_t s, uint32_t fp)
{
    if (!s || s >= ctx->memory_size) return -1;
    int hfd = std_handle_hfd(ctx, fp);
    if (hfd >= 0) {
        size_t len = strlen((const char *)(ctx->memory + s));
        ssize_t w = write(hfd, ctx->memory + s, len);
        return w < 0 ? -1 : (int32_t)w;
    }
    FILE *f = handle_to_file(fp);
    if (!f) return -1;
    return fputs((const char *)(ctx->memory + s), f);
}

/* ── flush / close-on-error / state ─────────────────────────────── */

int32_t yos_fflush(struct yos_exec_ctx *ctx, uint32_t fp)
{
    (void)ctx;
    if (fp == 0) return fflush(NULL);   /* flush all */
    FILE *f = handle_to_file(fp);
    if (!f) return 0;                    /* unknown handle: treat as no-op */
    return fflush(f) < 0 ? -errno : 0;
}

int32_t yos_feof   (struct yos_exec_ctx *ctx, uint32_t fp) { (void)ctx; FILE *f=handle_to_file(fp); return f?feof(f):0; }
int32_t yos_ferror (struct yos_exec_ctx *ctx, uint32_t fp) { (void)ctx; FILE *f=handle_to_file(fp); return f?ferror(f):0; }
int32_t yos_clearerr(struct yos_exec_ctx *ctx, uint32_t fp){ (void)ctx; FILE *f=handle_to_file(fp); if(f) clearerr(f); return 0; }
int32_t yos_fileno (struct yos_exec_ctx *ctx, uint32_t fp) { (void)ctx; FILE *f=handle_to_file(fp); return f?fileno(f):-1; }

/* ── seek/tell ─────────────────────────────────────────────────── */

int32_t yos_fseek (struct yos_exec_ctx *ctx, uint32_t fp, int32_t off, int32_t w) { (void)ctx; FILE *f=handle_to_file(fp); return f?fseek(f, off, w):-1; }
int32_t yos_fseeko(struct yos_exec_ctx *ctx, uint32_t fp, int64_t off, int32_t w) { (void)ctx; FILE *f=handle_to_file(fp); return f?fseeko(f, (off_t)off, w):-1; }
int32_t yos_ftell (struct yos_exec_ctx *ctx, uint32_t fp) { (void)ctx; FILE *f=handle_to_file(fp); return f?(int32_t)ftell(f):-1; }
int64_t yos_ftello(struct yos_exec_ctx *ctx, uint32_t fp) { (void)ctx; FILE *f=handle_to_file(fp); return f?(int64_t)ftello(f):-1; }
int32_t yos_rewind(struct yos_exec_ctx *ctx, uint32_t fp) { (void)ctx; FILE *f=handle_to_file(fp); if(f) rewind(f); return 0; }

/* ── buffering family ───────────────────────────────────────────── */

int32_t yos_setbuf(struct yos_exec_ctx *ctx, uint32_t fp, uint32_t buf)
{
    (void)ctx; (void)buf;
    FILE *f = handle_to_file(fp);
    if (!f) return 0;
    setbuf(f, NULL);   /* never expose guest buffer to host fwrite */
    return 0;
}

int32_t yos_setvbuf(struct yos_exec_ctx *ctx, uint32_t fp, uint32_t buf,
                    int32_t mode, uint32_t size)
{
    (void)ctx; (void)buf; (void)size;
    FILE *f = handle_to_file(fp);
    if (!f) return 0;
    return setvbuf(f, NULL, mode, 0);
}

int32_t yos_setbuffer(struct yos_exec_ctx *ctx, uint32_t fp, uint32_t buf, uint32_t size)
{
    (void)ctx; (void)buf; (void)size;
    FILE *f = handle_to_file(fp);
    if (!f) return 0;
    setbuffer(f, NULL, 0);
    return 0;
}

int32_t yos_setlinebuf(struct yos_exec_ctx *ctx, uint32_t fp)
{
    (void)ctx;
    FILE *f = handle_to_file(fp);
    if (!f) return 0;
    setlinebuf(f);
    return 0;
}

/* ── getline / getdelim ─────────────────────────────────────────── */

int32_t yos_getline(struct yos_exec_ctx *ctx, uint32_t lineptr, uint32_t n,
                    uint32_t fp)
{
    /* Need to allocate via guest malloc — can't expose host pointer.
     * Stub for now until the alloc side learns to hand out wasm
     * offsets to bridges that need them. */
    (void)ctx; (void)lineptr; (void)n; (void)fp;
    errno = ENOSYS;
    return -1;
}

int32_t yos_getdelim(struct yos_exec_ctx *ctx, uint32_t lineptr, uint32_t n,
                     int32_t delim, uint32_t fp)
{
    (void)ctx; (void)lineptr; (void)n; (void)delim; (void)fp;
    errno = ENOSYS;
    return -1;
}
