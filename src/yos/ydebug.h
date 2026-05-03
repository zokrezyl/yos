/*
 * YOS Debug Output
 *
 * In default builds, debug output is gated at RUNTIME by the YOS_DEBUG
 * env var (set YOS_DEBUG=1 to enable). The format string and arg
 * evaluation still cost something even when disabled.
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

#ifdef YOS_RELEASE

#define ydebug(fmt, ...) ((void)0)

#else

static int _ydebug_enabled = -1;  /* -1 = not initialized */

static inline int ydebug_enabled(void) {
    if (_ydebug_enabled < 0) {
        const char *env = getenv("YOS_DEBUG");
        _ydebug_enabled = (env && env[0] == '1');
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
