/* impl/syslog_extras.c — bridge surface for syslog + libutil bits
 * that aren't in our extractor's header walk.
 *
 * The codegen walks `api_top_headers` in src/yos/codegen/meson.build.
 * Neither <syslog.h> nor FreeBSD's <libutil.h> is on that list (they
 * pulled in machine-specific decls that broke extraction under the
 * wasm32 target), so the auto-bridge surface is missing every
 * function from those two headers. telnetd in particular needs:
 *
 *   openlog   — set syslog identity (no-op on yos; we log to stderr
 *               directly via syslog() below).
 *   closelog  — same — no-op.
 *   syslog    — variadic; format + write to stderr with a "[syslog]"
 *               prefix so it's easy to spot but won't get lost. Test
 *               rig only — a real production setup would route this
 *               through host syslog.
 *   login_tty — acquire a controlling TTY on the given fd. Maps
 *               1:1 to host login_tty (in <util.h> on glibc, in
 *               <libutil.h> on FreeBSD/darwin), which does
 *               setsid + ioctl(TIOCSCTTY) + dup2-to-0/1/2 + close.
 *   realhostname_sa — FreeBSD libutil reverse-DNS-with-fallback.
 *               Stubbed: write "unknown" + return 0. Telnetd uses
 *               this only to fill the audit hostname slot, so a
 *               literal "unknown" doesn't break anything functional.
 *
 * Linked at startup by yos_libc_extras_link_imports below, called
 * from src/yos/main.c right after the codegen's import-bind pass.
 */

#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>

/* login_tty lives in different headers per host. */
#if defined(__linux__)
#  include <pty.h>      /* glibc puts it here */
#elif defined(__APPLE__) || defined(__FreeBSD__)
#  include <util.h>     /* libutil */
#endif

#include "wasm3.h"
#include "m3_api_defs.h"
#include "m3_env.h"

#include "yos/types.h"
#include "yos/ydebug.h"
#include "impl/errno_helpers.h"

extern int yos_fd_get(struct yos_exec_ctx *, int);

/* ── env.openlog(const char *ident, int option, int facility) ─────
 * No-op: we don't maintain syslog state; syslog() below writes
 * straight to stderr. The wasm-side `ident` pointer is intentionally
 * ignored. */
static m3ApiRawFunction(m3_openlog)
{
    m3ApiGetArgMem(const char *, ident);
    m3ApiGetArg   (int32_t,      option);
    m3ApiGetArg   (int32_t,      facility);
    (void)ident; (void)option; (void)facility;
    (void)_ctx; (void)_mem;
    m3ApiSuccess();
}

static m3ApiRawFunction(m3_closelog)
{
    (void)_ctx; (void)_mem; (void)_sp;
    m3ApiSuccess();
}

/* ── env.syslog(int priority, const char *fmt, ...) ───────────────
 *
 * Variadic. Same shape as our printf bridge: the wasm caller
 * stages varargs in a va_list block reachable via the trailing
 * `va_ptr` arg the wasm32 ABI tacks on. We don't try to format the
 * varargs ourselves (would need to recreate format_one logic from
 * impl/printf.c here) — for telnetd's audit-trail use the priority
 * + format string + a "[syslog: …]" prefix is enough to see WHAT
 * the daemon is logging. If we ever need real %s/%d expansion we
 * can wire this through yos_vfprintf later. */
static m3ApiRawFunction(m3_syslog)
{
    m3ApiGetArg   (int32_t,      priority);
    m3ApiGetArgMem(const char *, fmt);
    /* Variadic trail: clang's wasm32 ABI passes a va_list_ptr as
     * the last argument. We don't decode it. */
    (void)priority;
    fprintf(stderr, "[syslog prio=%d] %s\n",
            (int)priority, fmt ? fmt : "(null)");
    (void)_ctx;
    m3ApiSuccess();
}

/* ── env.login_tty(int fd) ────────────────────────────────────────
 *
 * Acquire controlling TTY on the given wasm fd. Forwards to the
 * host login_tty which does the classic:
 *   setsid();
 *   ioctl(fd, TIOCSCTTY, 0);
 *   dup2(fd, 0); dup2(fd, 1); dup2(fd, 2);
 *   if (fd > 2) close(fd);
 * We translate the wasm fd to its host fd via yos_fd_get; on
 * failure return -1 with EBADF in the guest errno slot. */
static m3ApiRawFunction(m3_login_tty)
{
    m3ApiReturnType (int32_t);
    m3ApiGetArg     (int32_t, wfd);

    struct yos_exec_ctx *ctx =
        (struct yos_exec_ctx *)m3_GetUserData(runtime);
    int hfd = yos_fd_get(ctx, wfd);
    if (hfd < 0) {
        m3ApiReturn(yos_errno_neg(ctx, EBADF));
    }
    int r = login_tty(hfd);
    if (r < 0) {
        m3ApiReturn(yos_errno_neg(ctx, errno));
    }
    m3ApiReturn(r);
    m3ApiSuccess();
}

/* ── env.realhostname_sa(char *host, size_t hsize,
 *                        const struct sockaddr *sa, int salen)
 *
 * FreeBSD-libutil reverse-DNS with a numeric fallback. Telnetd uses
 * it to fill the connecting peer's hostname for audit / utmp slots.
 * For the test-rig we don't care about DNS — write "unknown" into
 * the guest buffer and return 0. */
static m3ApiRawFunction(m3_realhostname_sa)
{
    m3ApiReturnType (int32_t);
    m3ApiGetArgMem  (char *,                       host);
    m3ApiGetArg     (uint32_t,                     hsize);
    m3ApiGetArgMem  (const struct sockaddr *,      sa);
    m3ApiGetArg     (int32_t,                      salen);
    (void)sa; (void)salen; (void)_ctx;
    if (host && hsize >= 8) {
        memcpy(host, "unknown", 8);  /* incl. NUL */
    } else if (host && hsize > 0) {
        host[0] = '\0';
    }
    m3ApiReturn(0);
    m3ApiSuccess();
}

/* Called from src/yos/main.c after the codegen binds the auto-
 * generated env imports. These five aren't in the codegen surface
 * because <syslog.h> / <libutil.h> aren't in the extractor's
 * api_top_headers — adding them there would drag in machine-
 * specific decls that break the wasm32 extraction. Hand-binding
 * here is the targeted fix. */
void yos_syslog_extras_link_imports(IM3Module mod)
{
    M3Result r;
    r = m3_LinkRawFunction(mod, "env", "openlog",  "v(iii)",  m3_openlog);
    if (r && r != m3Err_functionLookupFailed)
        fprintf(stderr, "yos: link openlog: %s\n", r);
    r = m3_LinkRawFunction(mod, "env", "closelog", "v()",     m3_closelog);
    if (r && r != m3Err_functionLookupFailed)
        fprintf(stderr, "yos: link closelog: %s\n", r);
    r = m3_LinkRawFunction(mod, "env", "syslog",   "v(iii)",  m3_syslog);
    if (r && r != m3Err_functionLookupFailed)
        fprintf(stderr, "yos: link syslog: %s\n", r);
    r = m3_LinkRawFunction(mod, "env", "login_tty", "i(i)",   m3_login_tty);
    if (r && r != m3Err_functionLookupFailed)
        fprintf(stderr, "yos: link login_tty: %s\n", r);
    r = m3_LinkRawFunction(mod, "env", "realhostname_sa", "i(iiii)",
                           m3_realhostname_sa);
    if (r && r != m3Err_functionLookupFailed)
        fprintf(stderr, "yos: link realhostname_sa: %s\n", r);
}
