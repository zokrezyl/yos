#include "platform.h"   /* yos_plat_read / write / isatty / close */
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
#include <yos/ytrace/ytrace.h>
#include "impl/errno_helpers.h"

#define YOS_FILE_MAX 256

/* Per-slot state lives ON ctx->file_slots[] / ctx->file_wfds[] /
 * ctx->file_modes[] (types.h). Per-ctx storage is what lets a forked
 * child fclose its inherited handle without invalidating the parent's
 * still-live handle. fork additionally fdopen(dup(fileno(parent_fp)))
 * each live slot into the child's table so the underlying host FILE*
 * is independent in each side — that's what makes fclose on one side
 * leave the other side's host FILE* alive.
 *
 * Pre-fix: yos_fileno returned `fileno(host_FILE)` directly — the
 * raw host fd. The wasm guest passed that to env.fstat, which
 * routed it through yos_fd_get(wasm_fd) and tried to look up the
 * host fd as a wasm fd. fd_map[host_fd] was unset → EBADF →
 * "ssh nixem" failed with `fstat /Users/.../.ssh/config: Bad file
 * descriptor` after the underlying fopen had succeeded. */

extern int yos_fd_get(struct yos_exec_ctx *ctx, int wasm_fd);
extern int32_t yos_fd_alloc(struct yos_exec_ctx *ctx, int host_fd);
extern int32_t yos_fd_close(struct yos_exec_ctx *ctx, int32_t wfd);
extern const char *yos_path_resolve(struct yos_exec_ctx *ctx, const char *p);

FILE *yos_handle_to_file(struct yos_exec_ctx *ctx, uint32_t h)
{
    /* Pre-bound stream handles: 1 = stdin, 2 = stdout, 3 = stderr. */
    if (h == 1) return stdin;
    if (h == 2) return stdout;
    if (h == 3) return stderr;
    if (h < 4 || h >= YOS_FILE_MAX) return NULL;
    if (!ctx) return NULL;
    return (FILE *)ctx->file_slots[h];
}
#define handle_to_file(h) yos_handle_to_file(ctx, (h))

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

/* Register a host FILE* in the wasm-side handle table AND allocate
 * a wasm fd that wraps the host fd underneath. Two-step contract:
 *
 *   1. Slot the FILE* so fread / fwrite / etc. can find it by handle.
 *   2. Allocate a wasm fd in ctx->fd_map for the host fd inside the
 *      FILE — fileno() returns this wasm fd so subsequent stat / read
 *      / write / fcntl bridges resolve it correctly via yos_fd_get.
 *
 * Caller passes ctx so we can reach ctx->fd_map. wfd_out is the
 * allocated wasm fd (or -1 on alloc failure). */
uint32_t yos_alloc_file_handle_with_mode(struct yos_exec_ctx *ctx,
                                         FILE *f, const char *mode);

static uint32_t alloc_handle(struct yos_exec_ctx *ctx, FILE *f)
{
    return yos_alloc_file_handle_with_mode(ctx, f, NULL);
}

uint32_t yos_alloc_file_handle_with_mode(struct yos_exec_ctx *ctx,
                                         FILE *f, const char *mode)
{
    if (!f || !ctx) return 0;
    int hfd = fileno(f);
    int32_t wfd = (hfd >= 0) ? yos_fd_alloc(ctx, hfd) : -1;
    for (uint32_t i = 4; i < YOS_FILE_MAX; i++) {
        if (ctx->file_slots[i] == NULL) {
            ctx->file_slots[i] = f;
            ctx->file_wfds[i]  = wfd;
            /* Record mode so fork can fdopen on the dup'd fd with
             * the right access mode. mode==NULL means "we don't
             * know" (e.g. tmpfile()) — fall back to "r+" which
             * works for most callers post-fork. */
            const char *m = mode ? mode : "r+";
            size_t n = strlen(m);
            if (n >= sizeof(ctx->file_modes[i])) n = sizeof(ctx->file_modes[i]) - 1;
            memcpy(ctx->file_modes[i], m, n);
            ctx->file_modes[i][n] = '\0';
            return i;
        }
    }
    /* Out of slots — undo the wasm-fd alloc so we don't leak it. */
    if (wfd >= 0) yos_fd_close(ctx, wfd);
    return 0;
}

/* Public version for other impl files (impl/pwd.c::yos_tmpfile etc.)
 * that need to register a host FILE in the wasm-side handle table. */
uint32_t yos_alloc_file_handle(struct yos_exec_ctx *ctx, FILE *f)
{ return alloc_handle(ctx, f); }

static void free_handle(struct yos_exec_ctx *ctx, uint32_t h)
{
    if (h < 4 || h >= YOS_FILE_MAX || !ctx) return;
    int32_t wfd = ctx->file_wfds[h];
    ctx->file_slots[h] = NULL;
    ctx->file_wfds[h]  = -1;
    ctx->file_modes[h][0] = '\0';
    /* The host fd is closed by fclose() before we reach this point —
     * the wasm-fd slot just needs to be released (NOT close again,
     * which would EBADF). yos_fd_close hits yos_plat_close(hfd) again, so
     * we release the slot manually. */
    if (wfd >= 0) {
        extern void yos_fd_release_slot(struct yos_exec_ctx *ctx, int32_t wfd);
        yos_fd_release_slot(ctx, wfd);
    }
}

/* ── fopen / fclose / fdopen / freopen ─────────────────────────────── */

uint32_t yos_fopen(struct yos_exec_ctx *ctx, uint32_t path_off, uint32_t mode_off)
{
    if (!path_off || !mode_off) return 0;
    if (path_off >= ctx->memory_size || mode_off >= ctx->memory_size) return 0;
    const char *path_in = (const char *)(ctx->memory + path_off);
    const char *mode = (const char *)(ctx->memory + mode_off);
    /* Route through the per-ctx cwd resolver — yos never calls host
     * chdir(), so a raw fopen() on a relative path resolves against
     * the host process cwd, not the guest's. */
    const char *path = yos_path_resolve(ctx, path_in);
    /* Force binary mode if the guest didn't specify one. The wasm
     * guest expects POSIX byte-counting semantics — Windows fopen()
     * defaults to text mode and translates \n → \r\n on write, which
     * inflates the on-disk byte count and breaks the very next
     * fstat() / fread() round-trip. POSIX hosts treat 'b' as a no-op
     * already, so the only effect is on Windows. */
    char mbuf[16];
    const char *eff_mode = mode;
    {
        int has_b = 0, has_t = 0;
        for (int i = 0; mode[i] && i < (int)sizeof mbuf - 1; i++) {
            if (mode[i] == 'b') has_b = 1;
            if (mode[i] == 't') has_t = 1;
        }
        if (!has_b && !has_t) {
            int n = (int)strlen(mode);
            if (n + 2 <= (int)sizeof mbuf) {
                memcpy(mbuf, mode, n);
                mbuf[n]   = 'b';
                mbuf[n+1] = 0;
                eff_mode = mbuf;
            }
        }
    }
    FILE *f = fopen(path, eff_mode);
    if (!f) return 0;
    uint32_t h = yos_alloc_file_handle_with_mode(ctx, f, mode);
    if (!h) fclose(f);
    return h;
}

uint32_t yos_freopen(struct yos_exec_ctx *ctx, uint32_t path_off,
                    uint32_t mode_off, uint32_t fp)
{
    if (!path_off || !mode_off) return 0;
    FILE *f_old = handle_to_file(fp);
    const char *path_in = (const char *)(ctx->memory + path_off);
    const char *mode = (const char *)(ctx->memory + mode_off);
    const char *path = yos_path_resolve(ctx, path_in);
    FILE *f_new = freopen(path, mode, f_old ? f_old : NULL);
    if (!f_new) return 0;
    /* If reopened in-place, return the same handle. */
    if (f_new == f_old) return fp;
    if (fp >= 4 && fp < YOS_FILE_MAX) free_handle(ctx, fp);
    return alloc_handle(ctx, f_new);
}

uint32_t yos_fdopen(struct yos_exec_ctx *ctx, int32_t wfd, uint32_t mode_off)
{
    if (!ctx) return 0;
    int hfd = yos_fd_get(ctx, wfd);
    if (hfd < 0) return 0;
    if (!mode_off || mode_off >= ctx->memory_size) return 0;
    const char *mode = (const char *)(ctx->memory + mode_off);
    FILE *f = fdopen(hfd, mode);
    if (!f) return 0;
    /* fdopen transfers ownership of `hfd` to the FILE*: per POSIX, fclose
     * closes the underlying fd. Adopt the EXISTING wfd into the FILE
     * handle table — do NOT call yos_fd_alloc, which would point a
     * second wfd at the same hfd and leave the original wfd dangling at
     * a closed-then-recycled host fd after fclose. */
    extern void yos_fd_release_slot(struct yos_exec_ctx *ctx, int32_t wfd);
    for (uint32_t i = 4; i < YOS_FILE_MAX; i++) {
        if (ctx->file_slots[i] == NULL) {
            ctx->file_slots[i] = f;
            ctx->file_wfds[i]  = wfd;
            size_t n = strlen(mode);
            if (n >= sizeof(ctx->file_modes[i])) n = sizeof(ctx->file_modes[i]) - 1;
            memcpy(ctx->file_modes[i], mode, n);
            ctx->file_modes[i][n] = '\0';
            return i;
        }
    }
    /* Out of slots: fclose() will close hfd; release the wfd slot too
     * so the now-stale wfd reads EBADF instead of a recycled host fd. */
    fclose(f);
    yos_fd_release_slot(ctx, wfd);
    return 0;
}

int32_t yos_fclose(struct yos_exec_ctx *ctx, uint32_t fp)
{
    FILE *f = handle_to_file(fp);
    if (!f) return yos_errno_neg(ctx, EBADF);
    int r = fclose(f);
    if (fp >= 4 && fp < YOS_FILE_MAX) free_handle(ctx, fp);
    return yos_errno_check(ctx, r);
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

/* See vfs.c::yos_write for the rationale; same workaround needed
 * here because the wasm guest (zsh ZLE refresh, in particular) uses
 * fwrite() against stderr, which routes through yos_fwrite and
 * bypasses the write() path. Pure-0xff payloads ≤16 B going to a
 * terminal fd are dropped silently — they're a known fork-asyncify
 * leak from impl/proc.c. Set YOS_NO_FF_DROP=1 to disable. */
static inline int yos__drop_0xff_garbage(int hfd, const void *p, size_t n)
{
    if (n == 0 || n > 16 || hfd < 0) return 0;
    if (getenv("YOS_NO_FF_DROP")) return 0;
    if (yos_plat_isatty(hfd) != 1) return 0;
    const uint8_t *bp = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) if (bp[i] != 0xff) return 0;
    return 1;
}

uint32_t yos_fwrite(struct yos_exec_ctx *ctx, uint32_t buf, uint32_t size,
                    uint32_t nmemb, uint32_t fp)
{
    if (fp == 3 && size && nmemb) ctx->stderr_written_since_exec = 1;
    if (!size) return 0;
    if (buf + (uint64_t)size * nmemb > ctx->memory_size) return 0;
    /* Route stream handles through the per-ctx host fd so dup2'd
     * stdin/stdout/stderr actually take effect. See std_handle_hfd. */
    int hfd = std_handle_hfd(ctx, fp);
    if (hfd >= 0) {
        size_t total = (size_t)size * nmemb;
        if (yos__drop_0xff_garbage(hfd, ctx->memory + buf, total))
            return nmemb;
        ssize_t w = yos_plat_write(hfd, ctx->memory + buf, total);
        if (w <= 0) return 0;
        return (uint32_t)((size_t)w / size);
    }
    FILE *f = handle_to_file(fp);
    if (!f) return 0;
    if (f == stdout || f == stderr) {
        size_t total = (size_t)size * nmemb;
        if (yos__drop_0xff_garbage(fileno(f), ctx->memory + buf, total))
            return nmemb;
    }
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

/* Per-character stdio for stream handles 1/2/3 MUST use fd_map, not
 * host glibc's FILE* — otherwise bytes buffer in glibc's stderr/stdout
 * and later flush to the wrong host fd (see yos_fflush). One unsigned
 * byte read/written to/from the wasm-side fd_map'd host fd. */
static int stdio_fputc_via_fdmap(struct yos_exec_ctx *ctx, int c, uint32_t fp)
{
    int hfd = std_handle_hfd(ctx, fp);
    if (hfd < 0) return -2;
    unsigned char ch = (unsigned char)c;
    /* Honor the same fork-asyncify 0xff scratch workaround the fwrite
     * path applies. A single-byte fputc(0xff) reaches a TTY as a
     * stray invalid-UTF-8 codepoint that breaks column accounting
     * (the "backspace inserts a space" symptom under zsh ZLE). */
    if (yos__drop_0xff_garbage(hfd, &ch, 1)) return (int)ch;
    if (yos_plat_write(hfd, &ch, 1) != 1) return -1;
    return (int)ch;
}
static int stdio_fgetc_via_fdmap(struct yos_exec_ctx *ctx, uint32_t fp)
{
    int hfd = std_handle_hfd(ctx, fp);
    if (hfd < 0) return -2;
    unsigned char ch;
    ssize_t n = yos_plat_read(hfd, &ch, 1);
    return n == 1 ? (int)ch : -1;  /* EOF on n==0 or error on n<0 */
}

int32_t yos_fgetc(struct yos_exec_ctx *ctx, uint32_t fp) {
    int r = stdio_fgetc_via_fdmap(ctx, fp);
    if (r != -2) return r;
    FILE *f = handle_to_file(fp); return f?fgetc(f):-1;
}
int32_t yos_getc (struct yos_exec_ctx *ctx, uint32_t fp) {
    int r = stdio_fgetc_via_fdmap(ctx, fp);
    if (r != -2) return r;
    FILE *f = handle_to_file(fp); return f?getc(f):-1;
}
int32_t yos_getc_unlocked (struct yos_exec_ctx *ctx, uint32_t fp) {
    int r = stdio_fgetc_via_fdmap(ctx, fp);
    if (r != -2) return r;
    FILE *f = handle_to_file(fp); return f?getc_unlocked(f):-1;
}
int32_t yos_fputc(struct yos_exec_ctx *ctx, int32_t c, uint32_t fp) {
    if (fp == 3) ctx->stderr_written_since_exec = 1;
    int r = stdio_fputc_via_fdmap(ctx, c, fp);
    if (r != -2) return r;
    FILE *f = handle_to_file(fp); return f?fputc(c,f):-1;
}
int32_t yos_putc (struct yos_exec_ctx *ctx, int32_t c, uint32_t fp) {
    int r = stdio_fputc_via_fdmap(ctx, c, fp);
    if (r != -2) return r;
    FILE *f = handle_to_file(fp); return f?putc(c,f):-1;
}
int32_t yos_putc_unlocked (struct yos_exec_ctx *ctx, int32_t c, uint32_t fp) {
    int r = stdio_fputc_via_fdmap(ctx, c, fp);
    if (r != -2) return r;
    FILE *f = handle_to_file(fp); return f?putc_unlocked(c,f):-1;
}
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
    if (fp == 3) ctx->stderr_written_since_exec = 1;
    if (!s || s >= ctx->memory_size) return -1;
    int hfd = std_handle_hfd(ctx, fp);
    if (hfd >= 0) {
        size_t len = strlen((const char *)(ctx->memory + s));
        ssize_t w = yos_plat_write(hfd, ctx->memory + s, len);
        return w < 0 ? -1 : (int32_t)w;
    }
    FILE *f = handle_to_file(fp);
    if (!f) return -1;
    return fputs((const char *)(ctx->memory + s), f);
}

/* ── flush / close-on-error / state ─────────────────────────────── */

int32_t yos_fflush(struct yos_exec_ctx *ctx, uint32_t fp)
{
    /* Stream handles 1/2/3 are the wasm guest's stdin/stdout/stderr.
     * Their writes are routed through fd_map by yos_fwrite/yos_fputs
     * et al. — they go to the kernel via host write() directly, NOT
     * through host glibc's FILE* buffer. So a flush here is a no-op:
     * there's nothing left in any host buffer for these handles.
     *
     * WHY this matters: handle_to_file(3) returns the host process's
     * literal `stderr` FILE* (the one tied to host fd 2 = yos's own
     * terminal). If anyone earlier wrote BUFFERED bytes through that
     * FILE* (a stray yos_fputc, an unbridged libc call), this flush
     * would dump them on host fd 2 — bypassing the wasm guest's
     * fd_map[2] redirect and landing on the user's terminal regardless
     * of what the guest tried to do. zsh's "command not found"
     * message split exactly that way: the strerror() body wrote
     * through fd_map (correct) and the format-prefix wrote through
     * host glibc's buffer (then flushed to the wrong fd), producing
     * garbled output like "o such file or directoryzsh:1: N: …". */
    if (fp == 1 || fp == 2 || fp == 3) return 0;
    if (fp == 0) return yos_errno_check(ctx, fflush(NULL));   /* flush all */
    FILE *f = handle_to_file(fp);
    if (!f) return 0;                    /* unknown handle: treat as no-op */
    return yos_errno_check(ctx, fflush(f));
}

int32_t yos_feof   (struct yos_exec_ctx *ctx, uint32_t fp) { (void)ctx; FILE *f=handle_to_file(fp); return f?feof(f):0; }
int32_t yos_ferror (struct yos_exec_ctx *ctx, uint32_t fp) { (void)ctx; FILE *f=handle_to_file(fp); return f?ferror(f):0; }
int32_t yos_clearerr(struct yos_exec_ctx *ctx, uint32_t fp){ (void)ctx; FILE *f=handle_to_file(fp); if(f) clearerr(f); return 0; }
/* fileno(fp) — MUST return the WASM fd, not the host fd. The wasm
 * guest will then pass that to fstat / read / fcntl / close etc.,
 * all of which go through yos_fd_get to map the wasm fd to a host
 * fd. Returning the host fd directly (which old code did) made
 * fstat/read/fcntl EBADF every time — see commit message that
 * introduced struct yos_file_slot.wfd for the ssh bug context. */
int32_t yos_fileno(struct yos_exec_ctx *ctx, uint32_t fp)
{
    /* Stream handles 1/2/3 map to the per-ctx wasm fds 0/1/2
     * (stdin/stdout/stderr). */
    if (fp == 1) return 0;
    if (fp == 2) return 1;
    if (fp == 3) return 2;
    if (fp < 4 || fp >= YOS_FILE_MAX || !ctx) return -1;
    return ctx->file_wfds[fp];
}

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
    (void)ctx; (void)buf;
    FILE *f = handle_to_file(fp);
    if (!f) return 0;
    /* POSIX accepts setvbuf(f, NULL, mode, 0) as "set the buffering mode,
     * leave the internal buffer alone". MSVC's debug CRT asserts on
     * size < 2 even when buf is NULL — clamp to BUFSIZ so the call
     * lands in a benign code path on every host. */
    size_t hsz = size;
    if (hsz < 2) hsz = BUFSIZ;
    return setvbuf(f, NULL, mode, hsz);
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

extern uint32_t yos_malloc(struct yos_exec_ctx *ctx, uint32_t size);
extern uint32_t yos_realloc(struct yos_exec_ctx *ctx, uint32_t off, uint32_t newsize);

/* getdelim core. `lineptr_off` is a wasm pointer-to-uint32 (i.e. the
 * guest's `char **lineptr`); `n_off` is wasm pointer-to-uint32
 * (the `size_t *n`). On entry *lineptr may be 0 (we allocate via
 * yos_malloc), otherwise we use it as the seed buffer and grow it
 * via yos_realloc. We always update *lineptr / *n to reflect the
 * final buffer.
 *
 * Pre-fix ssh's ~/.ssh/config never got parsed because OpenSSH's
 * read_config_file uses getline() and our bridge returned -ENOSYS,
 * so every Host stanza was silently skipped. yos then handed the
 * un-resolved hostname (e.g. "nixem") straight to getaddrinfo. */
static int32_t do_getdelim(struct yos_exec_ctx *ctx,
                           uint32_t lineptr_off, uint32_t n_off,
                           int delim, uint32_t fp)
{
    FILE *f = handle_to_file(fp);
    if (!f) { errno = EBADF; return -1; }
    if (lineptr_off + 4 > ctx->memory_size || n_off + 4 > ctx->memory_size) {
        errno = EFAULT;
        return -1;
    }
    uint32_t buf_off = *(uint32_t *)(ctx->memory + lineptr_off);
    uint32_t buf_cap = *(uint32_t *)(ctx->memory + n_off);

    if (buf_off == 0 || buf_cap == 0) {
        buf_cap = 128;
        buf_off = yos_malloc(ctx, buf_cap);
        if (!buf_off) { errno = ENOMEM; return -1; }
    }

    size_t pos = 0;
    int c;
    for (;;) {
        c = fgetc(f);
        if (c == EOF) {
            if (pos == 0) {
                /* No data read AND EOF → return -1 per POSIX. errno
                 * stays 0 if it's a clean EOF, or has the read error. */
                *(uint32_t *)(ctx->memory + lineptr_off) = buf_off;
                *(uint32_t *)(ctx->memory + n_off)       = buf_cap;
                return -1;
            }
            break;
        }
        /* Need room for c AND the trailing NUL. */
        if (pos + 1 >= buf_cap) {
            uint32_t new_cap = buf_cap * 2;
            uint32_t new_off = yos_realloc(ctx, buf_off, new_cap);
            if (!new_off) { errno = ENOMEM; return -1; }
            buf_off = new_off;
            buf_cap = new_cap;
        }
        ctx->memory[buf_off + pos++] = (uint8_t)c;
        if (c == delim) break;
    }
    ctx->memory[buf_off + pos] = '\0';

    *(uint32_t *)(ctx->memory + lineptr_off) = buf_off;
    *(uint32_t *)(ctx->memory + n_off)       = buf_cap;
    return (int32_t)pos;
}

int32_t yos_getline(struct yos_exec_ctx *ctx, uint32_t lineptr, uint32_t n,
                    uint32_t fp)
{
    return do_getdelim(ctx, lineptr, n, '\n', fp);
}

int32_t yos_getdelim(struct yos_exec_ctx *ctx, uint32_t lineptr, uint32_t n,
                     int32_t delim, uint32_t fp)
{
    return do_getdelim(ctx, lineptr, n, delim, fp);
}
