/*
 * YOS debug output — now a thin shim over yos/ytrace/ytrace.h.
 *
 * Previously this header defined `ydebug()` itself as a simple
 * fprintf-to-stderr gated by `YTRACE_DEFAULT_ON`. We replaced that
 * with yetty's switchable-trace-point machinery, imported under
 * include/yos/ytrace/ + src/yos/ytrace/. The ytrace header defines
 * `ydebug`/`yinfo`/`ywarn`/`yerror`/`ytrace` plus `ytime_start`/
 * `ytime_report` for per-callsite timing. All existing call sites
 * just keep working.
 *
 * Quiet by default rule (CLAUDE.md): diagnostic output goes through
 * `ydebug()` — never raw `fprintf(stderr, "yos: ...")`. With ytrace
 * each trace point also gains independent runtime enable/disable so
 * `YTRACE_DEFAULT_ON=yes` lights everything up while specific lines
 * can be toggled programmatically via `ytrace_set_*_enabled()`.
 *
 * `ydebug_enabled()` is preserved as an inline that consults
 * ytrace's global default for callers that need to short-circuit
 * expensive arg evaluation (see vfs.c read/write hot paths).
 */

#ifndef YOS_YDEBUG_H
#define YOS_YDEBUG_H

#include <yos/ytrace/ytrace.h>

/* Hot-path query — used by impl/vfs.c to skip an expensive
 * snprintf+ydebug pair when tracing is off. Backed by a single
 * env-var lookup cached on first call. */
#include <stdlib.h>
#include <string.h>
static inline int ydebug_enabled(void)
{
    static int _cached = -1;
    if (_cached < 0) {
        const char *e = getenv("YTRACE_DEFAULT_ON");
        _cached = (e && (strcmp(e, "yes") == 0 || strcmp(e, "1") == 0));
    }
    return _cached;
}

#endif /* YOS_YDEBUG_H */
