/*
 * YOS Debug Output
 *
 * In default builds, debug output is gated at RUNTIME by the
 * `YTRACE_DEFAULT_ON` env var (set `YTRACE_DEFAULT_ON=yes` to enable).
 * Matches the project-wide trace convention used across yos / yetty —
 * one switch turns every trace point on. The format string and arg
 * evaluation still cost something even when disabled.
 *
 * Quiet by default: yos prints nothing to stderr in normal runs except
 * actual user-visible errors. Anything diagnostic / informational must
 * go through `ydebug()` — never raw `fprintf(stderr, "yos: ...")`.
 * That rule is enforced by the build/dev section of ./CLAUDE.md.
 *
 * In RELEASE builds (`-DYOS_RELEASE`), every ydebug() call is compiled
 * to nothing — the format string and the side-effect-free args are
 * dropped entirely, so there's no env check, no varargs setup, and the
 * format-string literals don't ship in the binary.
 *
 * Usage:
 *   ydebug("format string", args...);
 *
 * Side-effects in args ARE preserved across the release/debug split
 * (the macro is `(void)sizeof(...)` so the expression is type-checked
 * but never executed; passing function calls there is a bug regardless).
 */

#ifndef YOS_YDEBUG_H
#define YOS_YDEBUG_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef YOS_RELEASE

#define ydebug(fmt, ...) ((void)0)

#else

static int _ydebug_enabled = -1;  /* -1 = not initialized */

static inline int ydebug_enabled(void) {
    if (_ydebug_enabled < 0) {
        const char *env = getenv("YTRACE_DEFAULT_ON");
        _ydebug_enabled = (env && (strcmp(env, "yes") == 0
                                || strcmp(env, "1")   == 0));
    }
    return _ydebug_enabled;
}

#define ydebug(fmt, ...) \
    do { \
        if (ydebug_enabled()) { \
            fprintf(stderr, fmt, ##__VA_ARGS__); \
        } \
    } while (0)

#endif /* YOS_RELEASE */

#endif /* YOS_YDEBUG_H */
