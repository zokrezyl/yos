/* impl/pwd.c — getpwuid / getpwnam / getlogin and friends.
 *
 * Hand-written because FreeBSD's struct passwd is pointer-heavy
 * (5 char* fields) and each pointer must point INTO the wasm
 * guest's linear memory, not into glibc's static buffer. The
 * auto-bridge can't translate host pointers to wasm offsets.
 *
 * Approach mirrors POSIX: each call returns a pointer to a per-
 * ctx static buffer that holds the struct + all string contents.
 * The buffer is allocated once via yos_malloc() and reused on
 * every subsequent call — same semantics as libc's static
 * `_pw` storage, with the bonus that yos_malloc puts the buffer
 * at a stable wasm offset that the guest can dereference.
 *
 * The user's ssh bug is exactly this: yos used to have getpwuid
 * stubbed as -ENOSYS, so getpwuid(getuid()) returned NULL and
 * ssh printed "No user exists for uid 1000" before exec.
 *
 * FreeBSD wasm32 struct passwd layout (sizeof = 44):
 *   off  0: char *pw_name      (4 — wasm32 ptr)
 *   off  4: char *pw_passwd    (4)
 *   off  8: uid_t pw_uid       (4)
 *   off 12: gid_t pw_gid       (4)
 *   off 16: time_t pw_change   (4 — wasm32 time_t is 4 bytes)
 *   off 20: char *pw_class     (4)
 *   off 24: char *pw_gecos     (4)
 *   off 28: char *pw_dir       (4)
 *   off 32: char *pw_shell     (4)
 *   off 36: time_t pw_expire   (4)
 *   off 40: int pw_fields      (4)
 *
 * Same idea for struct group.
 */

#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>          /* tmpfile, fclose, FILE */
#include <pwd.h>
#include <grp.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <netdb.h>          /* gai_strerror, hstrerror */
#include <signal.h>         /* strsignal — declared in <string.h>
                             * with _GNU_SOURCE but pulling signal
                             * via posix.h ensures it's seen. */

#include "yos/types.h"
#include "yos/ydebug.h"

/* Forward decls for the auto-generated struct converter we use here.
 * Defined in build/<host>/src/yos/codegen/yos_struct_convert.c via
 * codegen/struct_convert.py. We don't #include the header because
 * impl/*.c don't have the codegen output dir on their -I path; the
 * forward decl + 44-byte size constant is all we need. */
extern void cv_tm_h2w(uint8_t *w, const struct tm *h);
#define CV_TM_GUEST_SZ 44u

extern uint32_t yos_malloc(struct yos_exec_ctx *ctx, uint32_t size);

#define WASM_PASSWD_SZ  44u
#define WASM_GROUP_SZ   16u   /* gr_name(4) + gr_passwd(4) + gr_gid(4) + gr_mem(4) */
#define PWD_BUF_SZ      512u
#define GRP_BUF_SZ      1024u
#define LOGIN_BUF_SZ    64u

/* Per-process static storage anchors. We keep one wasm-side buffer
 * per kind, reused across calls — same lifetime contract as libc's
 * static `_pw` / `_gr` slots. Allocated lazily on first call so
 * yos_malloc has a real ctx to allocate from. */
static uint32_t pwd_buf_off;        /* wasm offset of passwd buffer (struct+strs) */
static uint32_t grp_buf_off;        /* wasm offset of group buffer */
static uint32_t login_buf_off;      /* wasm offset of login-name buffer */

static uint32_t ensure_buf(struct yos_exec_ctx *ctx,
                           uint32_t *anchor, uint32_t want)
{
    if (*anchor) return *anchor;
    uint32_t off = yos_malloc(ctx, want);
    if (!off) return 0;
    *anchor = off;
    return off;
}

/* Copy NUL-terminated string `s` into the wasm slab starting at
 * `slab_base + *cursor`, return the wasm offset of the copy and
 * advance the cursor past the trailing NUL. NULL/empty strings get
 * the empty literal at the slab's first byte. Caller MUST pre-place
 * "" at slab_base[0] and start cursor at 1 for that to be valid. */
static uint32_t pack_string(struct yos_exec_ctx *ctx,
                            uint32_t slab_base, uint32_t slab_cap,
                            uint32_t *cursor, const char *s)
{
    if (!s) s = "";
    size_t n = strlen(s) + 1;
    if (*cursor + n > slab_cap) {
        /* No room — point at the slab's empty-string sentinel. */
        return slab_base;
    }
    memcpy(ctx->memory + slab_base + *cursor, s, n);
    uint32_t off = slab_base + *cursor;
    *cursor += (uint32_t)n;
    return off;
}

/* Write the 44-byte wasm-shape struct passwd at wasm offset `out`
 * from a host `struct passwd`. String fields are appended after the
 * struct in the same buffer (passed via slab_base/cap/cursor). */
static void write_wasm_passwd(struct yos_exec_ctx *ctx,
                              uint32_t out,
                              uint32_t slab_base, uint32_t slab_cap,
                              uint32_t *cursor,
                              const struct passwd *pw)
{
    uint8_t *w = ctx->memory + out;
    /* All zero first so missing fields read clean. */
    memset(w, 0, WASM_PASSWD_SZ);
    *(uint32_t *)(w +  0) = pack_string(ctx, slab_base, slab_cap, cursor, pw->pw_name);
    *(uint32_t *)(w +  4) = pack_string(ctx, slab_base, slab_cap, cursor, pw->pw_passwd);
    *(uint32_t *)(w +  8) = (uint32_t)pw->pw_uid;
    *(uint32_t *)(w + 12) = (uint32_t)pw->pw_gid;
    /* pw_change @ off 16 — host glibc has no pw_change; leave 0. */
    *(uint32_t *)(w + 20) = slab_base;  /* pw_class — empty */
    *(uint32_t *)(w + 24) = pack_string(ctx, slab_base, slab_cap, cursor, pw->pw_gecos);
    *(uint32_t *)(w + 28) = pack_string(ctx, slab_base, slab_cap, cursor, pw->pw_dir);
    *(uint32_t *)(w + 32) = pack_string(ctx, slab_base, slab_cap, cursor, pw->pw_shell);
    /* pw_expire @ off 36 — leave 0. */
    /* pw_fields @ off 40: bits for which fields were filled. We
     * always set NAME / UID / GID / GECOS / DIR / SHELL. */
    *(uint32_t *)(w + 40) = (1u<<0) | (1u<<1) | (1u<<2) | (1u<<3) |
                            (1u<<6) | (1u<<7) | (1u<<8);
}

/* Wasm-shape struct passwd: layout = 44 bytes; field 0 = pw_name. */
uint32_t yos_getpwuid(struct yos_exec_ctx *ctx, uint32_t uid)
{
    struct passwd *pw = getpwuid((uid_t)uid);
    if (!pw) {
        ydebug("getpwuid(%u) -> NULL\n", uid);
        return 0;
    }
    uint32_t buf = ensure_buf(ctx, &pwd_buf_off, PWD_BUF_SZ);
    if (!buf) { errno = ENOMEM; return 0; }
    /* Reserve the first byte for the empty-string sentinel that
     * pack_string returns when out of room. */
    ctx->memory[buf] = '\0';
    uint32_t cursor = WASM_PASSWD_SZ;
    if (cursor < 1) cursor = 1;
    write_wasm_passwd(ctx, buf, buf, PWD_BUF_SZ, &cursor, pw);
    ydebug("getpwuid(%u) -> name=%s uid=%u gid=%u dir=%s shell=%s\n",
           uid, pw->pw_name ? pw->pw_name : "(null)",
           pw->pw_uid, pw->pw_gid,
           pw->pw_dir  ? pw->pw_dir  : "(null)",
           pw->pw_shell? pw->pw_shell: "(null)");
    return buf;
}

uint32_t yos_getpwnam(struct yos_exec_ctx *ctx, uint32_t name_off)
{
    if (!name_off || name_off >= ctx->memory_size) return 0;
    const char *name = (const char *)(ctx->memory + name_off);
    struct passwd *pw = getpwnam(name);
    if (!pw) { ydebug("getpwnam(%s) -> NULL\n", name); return 0; }
    uint32_t buf = ensure_buf(ctx, &pwd_buf_off, PWD_BUF_SZ);
    if (!buf) return 0;
    ctx->memory[buf] = '\0';
    uint32_t cursor = WASM_PASSWD_SZ;
    write_wasm_passwd(ctx, buf, buf, PWD_BUF_SZ, &cursor, pw);
    ydebug("getpwnam(%s) -> uid=%u\n", name, pw->pw_uid);
    return buf;
}

/* getpwuid_r / getpwnam_r — caller-supplied result buffer + scratch.
 *
 *   int getpwuid_r(uid_t uid, struct passwd *pwd, char *buf,
 *                  size_t buflen, struct passwd **result);
 *
 * Wasm: pwd_off, buf_off, buflen, result_off (uint32 each). On
 * success, *result = pwd. On not-found, *result = NULL and rc = 0.
 */
int32_t yos_getpwuid_r(struct yos_exec_ctx *ctx, uint32_t uid,
                       uint32_t pwd_off, uint32_t buf_off,
                       uint32_t buflen, uint32_t result_off)
{
    if (!pwd_off || !result_off ||
        pwd_off >= ctx->memory_size || result_off >= ctx->memory_size)
        return EFAULT;
    struct passwd *pw = getpwuid((uid_t)uid);
    *(uint32_t *)(ctx->memory + result_off) = 0;
    if (!pw) return 0;
    if (buflen < 16 || !buf_off) {
        return ERANGE;
    }
    uint32_t cursor = 0;
    /* tiny sentinel so pack_string fallback is valid */
    ctx->memory[buf_off] = '\0';
    cursor = 1;
    write_wasm_passwd(ctx, pwd_off, buf_off, buflen, &cursor, pw);
    *(uint32_t *)(ctx->memory + result_off) = pwd_off;
    return 0;
}

int32_t yos_getpwnam_r(struct yos_exec_ctx *ctx, uint32_t name_off,
                       uint32_t pwd_off, uint32_t buf_off,
                       uint32_t buflen, uint32_t result_off)
{
    if (!name_off || !pwd_off || !result_off) return EFAULT;
    const char *name = (const char *)(ctx->memory + name_off);
    struct passwd *pw = getpwnam(name);
    *(uint32_t *)(ctx->memory + result_off) = 0;
    if (!pw) return 0;
    if (buflen < 16 || !buf_off) return ERANGE;
    uint32_t cursor = 0;
    ctx->memory[buf_off] = '\0';
    cursor = 1;
    write_wasm_passwd(ctx, pwd_off, buf_off, buflen, &cursor, pw);
    *(uint32_t *)(ctx->memory + result_off) = pwd_off;
    return 0;
}

/* getpwent / setpwent / endpwent — sequential walk over /etc/passwd.
 * Most programs we care about (ssh, zsh) only call getpwuid/getpwnam,
 * but provide functional implementations so e.g. `passwd -l` style
 * tools that walk the database don't return NULL straight away. */
uint32_t yos_getpwent(struct yos_exec_ctx *ctx)
{
    struct passwd *pw = getpwent();
    if (!pw) return 0;
    uint32_t buf = ensure_buf(ctx, &pwd_buf_off, PWD_BUF_SZ);
    if (!buf) return 0;
    ctx->memory[buf] = '\0';
    uint32_t cursor = WASM_PASSWD_SZ;
    write_wasm_passwd(ctx, buf, buf, PWD_BUF_SZ, &cursor, pw);
    return buf;
}

void yos_setpwent(struct yos_exec_ctx *ctx) { (void)ctx; setpwent(); }
void yos_endpwent(struct yos_exec_ctx *ctx) { (void)ctx; endpwent(); }

/* getlogin / getlogin_r — return the login name. Falls back to
 * `getpwuid(getuid())->pw_name` when getlogin returns NULL (the
 * usual "no controlling tty has a known login" case in CI / sandbox). */
uint32_t yos_getlogin(struct yos_exec_ctx *ctx)
{
    const char *l = getlogin();
    if (!l) {
        struct passwd *pw = getpwuid(getuid());
        if (pw && pw->pw_name) l = pw->pw_name;
        else l = "yos";
    }
    uint32_t buf = ensure_buf(ctx, &login_buf_off, LOGIN_BUF_SZ);
    if (!buf) return 0;
    size_t n = strlen(l);
    if (n >= LOGIN_BUF_SZ) n = LOGIN_BUF_SZ - 1;
    memcpy(ctx->memory + buf, l, n);
    ctx->memory[buf + n] = '\0';
    return buf;
}

int32_t yos_getlogin_r(struct yos_exec_ctx *ctx, uint32_t buf_off,
                       uint32_t buflen)
{
    if (!buf_off) return EFAULT;
    const char *l = getlogin();
    if (!l) {
        struct passwd *pw = getpwuid(getuid());
        if (pw && pw->pw_name) l = pw->pw_name;
        else l = "yos";
    }
    size_t n = strlen(l);
    if (n + 1 > buflen) return ERANGE;
    memcpy(ctx->memory + buf_off, l, n + 1);
    return 0;
}

/* ── struct group ─────────────────────────────────────────────────
 *
 * FreeBSD wasm32 struct group layout (sizeof = 16):
 *   off  0: char  *gr_name      (4)
 *   off  4: char  *gr_passwd    (4)
 *   off  8: gid_t  gr_gid       (4)
 *   off 12: char **gr_mem       (4 — pointer to array of char*)
 *
 * gr_mem is a NULL-terminated array of member-name pointers. Each
 * char* in the array is itself a wasm offset. We pack the entire
 * structure + name strings + member-pointer array + member name
 * strings into one shared buffer. */

static void write_wasm_group(struct yos_exec_ctx *ctx,
                             uint32_t out,
                             uint32_t slab_base, uint32_t slab_cap,
                             uint32_t *cursor,
                             const struct group *gr)
{
    uint8_t *w = ctx->memory + out;
    memset(w, 0, WASM_GROUP_SZ);
    *(uint32_t *)(w + 0) = pack_string(ctx, slab_base, slab_cap, cursor, gr->gr_name);
    *(uint32_t *)(w + 4) = pack_string(ctx, slab_base, slab_cap, cursor, gr->gr_passwd);
    *(uint32_t *)(w + 8) = (uint32_t)gr->gr_gid;

    /* Build the gr_mem array. Count members first. */
    size_t nmem = 0;
    if (gr->gr_mem) while (gr->gr_mem[nmem]) nmem++;
    /* Align cursor to 4 bytes for the pointer array. */
    if (*cursor & 3) *cursor = (*cursor + 3) & ~3u;
    uint32_t arr_off = slab_base + *cursor;
    if (*cursor + (nmem + 1) * 4 > slab_cap) {
        /* Out of room for the array — write NULL. */
        return;
    }
    *cursor += (uint32_t)((nmem + 1) * 4);
    /* Copy each member name and write its offset into the array. */
    for (size_t i = 0; i < nmem; i++) {
        uint32_t off = pack_string(ctx, slab_base, slab_cap, cursor,
                                   gr->gr_mem[i]);
        *(uint32_t *)(ctx->memory + arr_off + i * 4) = off;
    }
    *(uint32_t *)(ctx->memory + arr_off + nmem * 4) = 0;  /* NULL term */
    *(uint32_t *)(w + 12) = arr_off;
}

uint32_t yos_getgrgid(struct yos_exec_ctx *ctx, uint32_t gid)
{
    struct group *gr = getgrgid((gid_t)gid);
    if (!gr) { ydebug("getgrgid(%u) -> NULL\n", gid); return 0; }
    uint32_t buf = ensure_buf(ctx, &grp_buf_off, GRP_BUF_SZ);
    if (!buf) return 0;
    ctx->memory[buf] = '\0';
    uint32_t cursor = WASM_GROUP_SZ;
    write_wasm_group(ctx, buf, buf, GRP_BUF_SZ, &cursor, gr);
    ydebug("getgrgid(%u) -> name=%s\n", gid,
           gr->gr_name ? gr->gr_name : "(null)");
    return buf;
}

uint32_t yos_getgrnam(struct yos_exec_ctx *ctx, uint32_t name_off)
{
    if (!name_off || name_off >= ctx->memory_size) return 0;
    const char *name = (const char *)(ctx->memory + name_off);
    struct group *gr = getgrnam(name);
    if (!gr) return 0;
    uint32_t buf = ensure_buf(ctx, &grp_buf_off, GRP_BUF_SZ);
    if (!buf) return 0;
    ctx->memory[buf] = '\0';
    uint32_t cursor = WASM_GROUP_SZ;
    write_wasm_group(ctx, buf, buf, GRP_BUF_SZ, &cursor, gr);
    return buf;
}

int32_t yos_getgrgid_r(struct yos_exec_ctx *ctx, uint32_t gid,
                       uint32_t grp_off, uint32_t buf_off,
                       uint32_t buflen, uint32_t result_off)
{
    if (!grp_off || !result_off) return EFAULT;
    struct group *gr = getgrgid((gid_t)gid);
    *(uint32_t *)(ctx->memory + result_off) = 0;
    if (!gr) return 0;
    if (buflen < 32 || !buf_off) return ERANGE;
    uint32_t cursor = 0;
    ctx->memory[buf_off] = '\0';
    cursor = 1;
    write_wasm_group(ctx, grp_off, buf_off, buflen, &cursor, gr);
    *(uint32_t *)(ctx->memory + result_off) = grp_off;
    return 0;
}

int32_t yos_getgrnam_r(struct yos_exec_ctx *ctx, uint32_t name_off,
                       uint32_t grp_off, uint32_t buf_off,
                       uint32_t buflen, uint32_t result_off)
{
    if (!name_off || !grp_off || !result_off) return EFAULT;
    const char *name = (const char *)(ctx->memory + name_off);
    struct group *gr = getgrnam(name);
    *(uint32_t *)(ctx->memory + result_off) = 0;
    if (!gr) return 0;
    if (buflen < 32 || !buf_off) return ERANGE;
    uint32_t cursor = 0;
    ctx->memory[buf_off] = '\0';
    cursor = 1;
    write_wasm_group(ctx, grp_off, buf_off, buflen, &cursor, gr);
    *(uint32_t *)(ctx->memory + result_off) = grp_off;
    return 0;
}

uint32_t yos_getgrent(struct yos_exec_ctx *ctx)
{
    struct group *gr = getgrent();
    if (!gr) return 0;
    uint32_t buf = ensure_buf(ctx, &grp_buf_off, GRP_BUF_SZ);
    if (!buf) return 0;
    ctx->memory[buf] = '\0';
    uint32_t cursor = WASM_GROUP_SZ;
    write_wasm_group(ctx, buf, buf, GRP_BUF_SZ, &cursor, gr);
    return buf;
}

void yos_setgrent(struct yos_exec_ctx *ctx) { (void)ctx; setgrent(); }
void yos_endgrent(struct yos_exec_ctx *ctx) { (void)ctx; endgrent(); }

/* ── time-string family (gmtime / localtime / ctime / asctime) ───
 *
 * All four return `char *` or `struct tm *` to a static buffer in
 * libc. Same wasm-side problem as getpwuid: the host pointer can't
 * be returned directly. Allocate per-ctx static slots, copy into
 * them, return the wasm offset.
 *
 * Auto-bridge stubs them as -ENOSYS today (see hooks.yaml stub:);
 * here we route them via custom_proc instead. */

static uint32_t tm_buf_off;       /* CV_TM_GUEST_SZ = 44 bytes */
static uint32_t timestr_buf_off;  /* "Wed Jun 30 21:49:08 1993\n\0" = 26 chars */
#define TIMESTR_BUF_SZ  64u

uint32_t yos_gmtime(struct yos_exec_ctx *ctx, uint32_t time_off)
{
    if (!time_off || time_off + 4 > ctx->memory_size) {
        errno = EFAULT; return 0;
    }
    time_t t = (time_t)*(int32_t *)(ctx->memory + time_off);
    struct tm tm;
    if (!gmtime_r(&t, &tm)) return 0;
    uint32_t buf = ensure_buf(ctx, &tm_buf_off, CV_TM_GUEST_SZ);
    if (!buf) return 0;
    cv_tm_h2w(ctx->memory + buf, &tm);
    return buf;
}

uint32_t yos_localtime(struct yos_exec_ctx *ctx, uint32_t time_off)
{
    if (!time_off || time_off + 4 > ctx->memory_size) {
        errno = EFAULT; return 0;
    }
    time_t t = (time_t)*(int32_t *)(ctx->memory + time_off);
    struct tm tm;
    if (!localtime_r(&t, &tm)) return 0;
    uint32_t buf = ensure_buf(ctx, &tm_buf_off, CV_TM_GUEST_SZ);
    if (!buf) return 0;
    cv_tm_h2w(ctx->memory + buf, &tm);
    return buf;
}

uint32_t yos_ctime(struct yos_exec_ctx *ctx, uint32_t time_off)
{
    if (!time_off || time_off + 4 > ctx->memory_size) {
        errno = EFAULT; return 0;
    }
    time_t t = (time_t)*(int32_t *)(ctx->memory + time_off);
    char hostbuf[64];
    if (!ctime_r(&t, hostbuf)) return 0;
    uint32_t buf = ensure_buf(ctx, &timestr_buf_off, TIMESTR_BUF_SZ);
    if (!buf) return 0;
    size_t n = strlen(hostbuf);
    if (n >= TIMESTR_BUF_SZ) n = TIMESTR_BUF_SZ - 1;
    memcpy(ctx->memory + buf, hostbuf, n);
    ctx->memory[buf + n] = '\0';
    return buf;
}

int32_t yos_ctime_r(struct yos_exec_ctx *ctx, uint32_t time_off,
                    uint32_t buf_off)
{
    if (!time_off || !buf_off ||
        time_off + 4 > ctx->memory_size ||
        buf_off + 26 > ctx->memory_size) {
        errno = EFAULT; return 0;
    }
    time_t t = (time_t)*(int32_t *)(ctx->memory + time_off);
    char hostbuf[64];
    if (!ctime_r(&t, hostbuf)) return 0;
    size_t n = strlen(hostbuf);
    if (n >= 26) n = 25;
    memcpy(ctx->memory + buf_off, hostbuf, n);
    ctx->memory[buf_off + n] = '\0';
    /* ctime_r returns the buf pointer (= buf_off as wasm offset). */
    return (int32_t)buf_off;
}

uint32_t yos_asctime(struct yos_exec_ctx *ctx, uint32_t tm_off)
{
    if (!tm_off || tm_off + CV_TM_GUEST_SZ > ctx->memory_size) {
        errno = EFAULT; return 0;
    }
    /* Decode wasm-side struct tm into a host struct tm. */
    const uint8_t *w = ctx->memory + tm_off;
    struct tm tm = {0};
    tm.tm_sec   = *(int32_t *)(w +  0);
    tm.tm_min   = *(int32_t *)(w +  4);
    tm.tm_hour  = *(int32_t *)(w +  8);
    tm.tm_mday  = *(int32_t *)(w + 12);
    tm.tm_mon   = *(int32_t *)(w + 16);
    tm.tm_year  = *(int32_t *)(w + 20);
    tm.tm_wday  = *(int32_t *)(w + 24);
    tm.tm_yday  = *(int32_t *)(w + 28);
    tm.tm_isdst = *(int32_t *)(w + 32);
    char hostbuf[64];
    if (!asctime_r(&tm, hostbuf)) return 0;
    uint32_t buf = ensure_buf(ctx, &timestr_buf_off, TIMESTR_BUF_SZ);
    if (!buf) return 0;
    size_t n = strlen(hostbuf);
    if (n >= TIMESTR_BUF_SZ) n = TIMESTR_BUF_SZ - 1;
    memcpy(ctx->memory + buf, hostbuf, n);
    ctx->memory[buf + n] = '\0';
    return buf;
}

int32_t yos_asctime_r(struct yos_exec_ctx *ctx, uint32_t tm_off,
                      uint32_t buf_off)
{
    if (!tm_off || !buf_off) { errno = EFAULT; return 0; }
    const uint8_t *w = ctx->memory + tm_off;
    struct tm tm = {0};
    tm.tm_sec   = *(int32_t *)(w +  0);
    tm.tm_min   = *(int32_t *)(w +  4);
    tm.tm_hour  = *(int32_t *)(w +  8);
    tm.tm_mday  = *(int32_t *)(w + 12);
    tm.tm_mon   = *(int32_t *)(w + 16);
    tm.tm_year  = *(int32_t *)(w + 20);
    tm.tm_wday  = *(int32_t *)(w + 24);
    tm.tm_yday  = *(int32_t *)(w + 28);
    tm.tm_isdst = *(int32_t *)(w + 32);
    char hostbuf[64];
    if (!asctime_r(&tm, hostbuf)) return 0;
    size_t n = strlen(hostbuf);
    if (n >= 26) n = 25;
    memcpy(ctx->memory + buf_off, hostbuf, n);
    ctx->memory[buf_off + n] = '\0';
    return (int32_t)buf_off;
}

/* ── string-returning helpers (static-buffer pattern) ────────────
 *
 *   strerror, gai_strerror, hstrerror, strsignal — all return
 *   a libc-internal `const char *`. Same wasm-side problem as
 *   getlogin: the host pointer can't cross the boundary; we copy
 *   to a per-ctx wasm buffer and return its offset.
 *
 *   The buffers are reused across calls, matching libc's static-
 *   storage semantics. Each function gets its own slot so calls
 *   don't trample each other.
 */

static uint32_t errstr_buf_off;
static uint32_t gaistr_buf_off;
static uint32_t hstr_buf_off;
static uint32_t signam_buf_off;
#define ERRSTR_BUF_SZ  128u

static uint32_t copy_const_to_wasm(struct yos_exec_ctx *ctx,
                                   uint32_t *anchor, const char *s)
{
    if (!s) s = "";
    uint32_t buf = ensure_buf(ctx, anchor, ERRSTR_BUF_SZ);
    if (!buf) return 0;
    size_t n = strlen(s);
    if (n >= ERRSTR_BUF_SZ) n = ERRSTR_BUF_SZ - 1;
    memcpy(ctx->memory + buf, s, n);
    ctx->memory[buf + n] = '\0';
    return buf;
}

uint32_t yos_strerror(struct yos_exec_ctx *ctx, int32_t errnum)
{
    return copy_const_to_wasm(ctx, &errstr_buf_off, strerror(errnum));
}

uint32_t yos_gai_strerror(struct yos_exec_ctx *ctx, int32_t errnum)
{
    return copy_const_to_wasm(ctx, &gaistr_buf_off, gai_strerror(errnum));
}

uint32_t yos_hstrerror(struct yos_exec_ctx *ctx, int32_t errnum)
{
    return copy_const_to_wasm(ctx, &hstr_buf_off, hstrerror(errnum));
}

uint32_t yos_strsignal(struct yos_exec_ctx *ctx, int32_t signum)
{
    return copy_const_to_wasm(ctx, &signam_buf_off, strsignal(signum));
}

/* ── strtok / strtok_r / strsep (pointer-into-input return) ─────
 *
 * These return a pointer to a substring of the INPUT buffer (which
 * lives in wasm memory). The bridge can compute the wasm offset by:
 *   wasm_offset = input_wasm_offset + (host_result - host_input).
 *
 * The wasm input pointer is `s_off`; we resolve it to a host
 * pointer, call host strtok which writes a NUL into the buffer and
 * returns a pointer to the token start (or NULL).
 */

uint32_t yos_strtok(struct yos_exec_ctx *ctx, uint32_t s_off, uint32_t delim_off)
{
    /* strtok keeps state across calls in a libc-internal slot.
     * First call: s_off != 0; subsequent calls: s_off == 0 (NULL).
     * We honour that exactly. */
    char *s_h = s_off ? (char *)(ctx->memory + s_off) : NULL;
    const char *delim_h = delim_off ? (const char *)(ctx->memory + delim_off) : "";
    char *r = strtok(s_h, delim_h);
    if (!r) return 0;
    return (uint32_t)(r - (char *)ctx->memory);
}

uint32_t yos_strtok_r(struct yos_exec_ctx *ctx, uint32_t s_off,
                      uint32_t delim_off, uint32_t saveptr_off)
{
    if (!saveptr_off || saveptr_off + 4 > ctx->memory_size)
        return 0;
    /* The wasm-side `*saveptr` is a wasm offset; convert it to a
     * host pointer for strtok_r, which we then convert back. */
    uint32_t *wsave = (uint32_t *)(ctx->memory + saveptr_off);
    char *host_save = *wsave ? (char *)(ctx->memory + *wsave) : NULL;
    char *s_h = s_off ? (char *)(ctx->memory + s_off) : NULL;
    const char *delim_h = delim_off ? (const char *)(ctx->memory + delim_off) : "";
    char *r = strtok_r(s_h, delim_h, &host_save);
    /* Persist updated saveptr back to wasm. */
    *wsave = host_save ? (uint32_t)(host_save - (char *)ctx->memory) : 0;
    if (!r) return 0;
    return (uint32_t)(r - (char *)ctx->memory);
}

uint32_t yos_strsep(struct yos_exec_ctx *ctx, uint32_t stringp_off,
                    uint32_t delim_off)
{
    if (!stringp_off || stringp_off + 4 > ctx->memory_size) return 0;
    uint32_t *wstringp = (uint32_t *)(ctx->memory + stringp_off);
    char *host_str = *wstringp ? (char *)(ctx->memory + *wstringp) : NULL;
    const char *delim_h = delim_off ? (const char *)(ctx->memory + delim_off) : "";
    char *r = strsep(&host_str, delim_h);
    *wstringp = host_str ? (uint32_t)(host_str - (char *)ctx->memory) : 0;
    if (!r) return 0;
    return (uint32_t)(r - (char *)ctx->memory);
}

/* strptime — returns char* one past the last parsed character (or
 * NULL on failure). Same pointer-into-input convention as strtok. */
uint32_t yos_strptime(struct yos_exec_ctx *ctx, uint32_t buf_off,
                      uint32_t fmt_off, uint32_t tm_off)
{
    if (!buf_off || !fmt_off) return 0;
    const char *buf = (const char *)(ctx->memory + buf_off);
    const char *fmt = (const char *)(ctx->memory + fmt_off);
    struct tm tm = {0};
    char *r = strptime(buf, fmt, &tm);
    if (!r) return 0;
    if (tm_off) {
        cv_tm_h2w(ctx->memory + tm_off, &tm);
    }
    return buf_off + (uint32_t)(r - buf);
}

/* posix_memalign lives in impl/alloc.c — see yos_posix_memalign there. */

/* tmpfile() — open an unlinked temp file, register its host FILE*
 * in impl/file.c's handle table. The auto-bridge stubs this
 * because the return type FILE* is complex; the handle table makes
 * it trivial. */
extern uint32_t yos_alloc_file_handle(FILE *f);

uint32_t yos_tmpfile(struct yos_exec_ctx *ctx)
{
    (void)ctx;
    FILE *f = tmpfile();
    if (!f) return 0;
    uint32_t h = yos_alloc_file_handle(f);
    if (!h) fclose(f);
    return h;
}
