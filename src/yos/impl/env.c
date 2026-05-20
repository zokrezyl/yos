/* impl/env.c — getenv / setenv / unsetenv / putenv / clearenv backed
 * by a per-ctx environment store kept in the wasm guest's linear
 * memory.
 *
 * The catch: getenv returns `char *`, which the guest must be able
 * to dereference. Returning a HOST pointer (to glibc's internal
 * environ table) doesn't work — guest's `ctx->memory + offset` model
 * has no idea where the host's heap lives. So we keep the env strings
 * INSIDE the wasm linear memory: an array of (name=value) strings
 * malloc'd via yos_malloc (which lives in the guest's mimalloc
 * arena). yos_setenv copies the new strings there, yos_getenv looks
 * up by name and returns the cached offset.
 *
 * Start state: at first getenv/setenv we walk the host `environ` and
 * copy every entry into the wasm side, so the guest sees the same
 * environment yos was started with.
 *
 * Layout: a parallel array of {name_off, value_off} entries; both
 * offsets point into the wasm linear memory and are stable until
 * the entry is unset / clearenv is called. This is enough for the
 * FreeBSD clearenv test, the simple plus-pop pattern nvim uses, and
 * most other programs.
 */

#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include "yos/types.h"
#include <yos/ytrace/ytrace.h>
#include "impl/errno_helpers.h"

extern uint32_t yos_malloc(struct yos_exec_ctx *ctx, uint32_t size);
extern void     yos_free  (struct yos_exec_ctx *ctx, uint32_t off);
extern char   **environ;

#define YOS_ENV_MAX  512

struct yos_env_entry {
    uint32_t name_off;   /* offset of the name copy in wasm memory */
    uint32_t value_off;  /* offset of the value copy */
    uint32_t name_len;
};

struct yos_env_store {
    struct yos_env_entry e[YOS_ENV_MAX];
    int    count;
    int    initialised;
};

/* One static store per process — every fork gets its own ctx but the
 * env doesn't track per-ctx. If we end up needing per-process envs,
 * move this onto struct yos_exec_ctx. */
static struct yos_env_store g_env;

/* Copy a host string into the wasm linear memory via yos_malloc.
 * Returns the wasm offset, or 0 on failure. */
static uint32_t copy_to_wasm(struct yos_exec_ctx *ctx, const char *s)
{
    if (!s) return 0;
    size_t n = strlen(s) + 1;
    uint32_t off = yos_malloc(ctx, (uint32_t)n);
    if (!off) return 0;
    memcpy(ctx->memory + off, s, n);
    return off;
}

static int find_entry(const struct yos_env_store *st,
                      struct yos_exec_ctx *ctx,
                      const char *name)
{
    size_t nlen = strlen(name);
    for (int i = 0; i < st->count; i++) {
        if (st->e[i].name_off == 0) continue;
        if (st->e[i].name_len != nlen) continue;
        if (memcmp(ctx->memory + st->e[i].name_off, name, nlen) == 0)
            return i;
    }
    return -1;
}

/* Walk host environ and copy entries into wasm. Used both for
 * lazy-init on first getenv/setenv AND from yos_env_reload() to
 * reset state between FreeBSD ATF test cases (their atf-runner
 * forks per-test; we run them serially in one process, so without
 * an explicit reload hook the previous test's clearenv() would
 * leave the env empty for the next test). */
static void env_load_from_host(struct yos_exec_ctx *ctx)
{
    if (!environ) return;
    for (char **p = environ; *p && g_env.count < YOS_ENV_MAX; p++) {
        const char *eq = strchr(*p, '=');
        if (!eq) continue;
        size_t nlen = (size_t)(eq - *p);
        char nbuf[256];
        if (nlen >= sizeof nbuf) continue;
        memcpy(nbuf, *p, nlen);
        nbuf[nlen] = 0;
        uint32_t name_off  = copy_to_wasm(ctx, nbuf);
        uint32_t value_off = copy_to_wasm(ctx, eq + 1);
        if (!name_off || !value_off) continue;
        g_env.e[g_env.count].name_off  = name_off;
        g_env.e[g_env.count].value_off = value_off;
        g_env.e[g_env.count].name_len  = (uint32_t)nlen;
        g_env.count++;
    }
}

static void env_init_once(struct yos_exec_ctx *ctx)
{
    if (g_env.initialised) return;
    g_env.initialised = 1;
    env_load_from_host(ctx);
}

/* Test-only: drop all known entries and re-pull from host environ.
 * Bound as `env.__yos_env_reload` — atf-runner-style forks would
 * give each test case a fresh env; we approximate by reloading. */
void yos_env_reload(struct yos_exec_ctx *ctx)
{
    /* Free the wasm-side string buffers we allocated. */
    for (int i = 0; i < g_env.count; i++) {
        if (g_env.e[i].name_off)  yos_free(ctx, g_env.e[i].name_off);
        if (g_env.e[i].value_off) yos_free(ctx, g_env.e[i].value_off);
    }
    memset(&g_env, 0, sizeof g_env);
    g_env.initialised = 1;
    env_load_from_host(ctx);
}

uint32_t yos_getenv(struct yos_exec_ctx *ctx, uint32_t name_off)
{
    env_init_once(ctx);
    if (!name_off || name_off >= ctx->memory_size) return 0;
    const char *name = (const char *)(ctx->memory + name_off);
    int idx = find_entry(&g_env, ctx, name);
    ydebug("getenv(\"%s\") -> %s (idx=%d)\n", name,
           idx >= 0 ? "found" : "NULL", idx);
    if (idx < 0) return 0;
    return g_env.e[idx].value_off;
}

int32_t yos_setenv(struct yos_exec_ctx *ctx, uint32_t name_off,
                   uint32_t value_off, int32_t overwrite)
{
    env_init_once(ctx);
    if (!name_off || !value_off ||
        name_off >= ctx->memory_size || value_off >= ctx->memory_size)
        return yos_errno_neg(ctx, EINVAL);

    const char *name  = (const char *)(ctx->memory + name_off);
    const char *value = (const char *)(ctx->memory + value_off);
    if (!*name || strchr(name, '=') != NULL)
        return yos_errno_neg(ctx, EINVAL);

    /* YTRACE_* env vars take effect immediately on the host trace
     * system, so the guest-side `ytrace` wrapper (a tiny setenv +
     * execvp wasm program) can flip tracing on for the child it's
     * about to exec. Without this, setenv() only updates the
     * per-process g_env store and the next exec'd child inherits
     * it for its own getenv() lookups — but the HOST'S ytrace
     * registry (process-wide, not per-ctx) never learns about the
     * change because that lives outside the guest's env model. */
    if (strcmp(name, "YTRACE_DEFAULT_ON") == 0) {
        extern void ytrace_set_all_enabled(bool);
        bool on = (strcmp(value, "yes") == 0 || strcmp(value, "1") == 0 ||
                   strcmp(value, "true") == 0);
        ytrace_set_all_enabled(on);
    } else if (strcmp(name, "YTRACE_FILE_PREFIX") == 0) {
        /* Propagate to host setenv so ytrace.c's per-thread file
         * re-open (driven by yos_ytrace_set_comm on each new comm)
         * picks up the new prefix. */
        setenv("YTRACE_FILE_PREFIX", value, 1);
    } else if (strcmp(name, "YPERF") == 0) {
        extern void yperf_set_enabled(bool);
        extern void yperf_dump_and_reset(void);
        if (strcmp(value, "stop") == 0) {
            /* "stop" → flush captured profile to YPERF_FILE and
             * disable recording. Used by the guest-side `yperf`
             * wrapper after wait()-ing for the child it was
             * profiling, so the dump is bounded to that child's
             * lifetime. The next setenv("YPERF","yes") begins a
             * fresh capture. */
            yperf_dump_and_reset();
        } else {
            bool on = (strcmp(value, "yes") == 0 || strcmp(value, "1") == 0 ||
                       strcmp(value, "true") == 0);
            yperf_set_enabled(on);
        }
    } else if (strcmp(name, "YPERF_FILE") == 0) {
        /* yperf reads YPERF_FILE at dump time via getenv, so push
         * to the host env so the eventual atexit-driven dump finds
         * it. Mirrors the YTRACE_FILE_PREFIX path above. */
        setenv("YPERF_FILE", value, 1);
    } else if (strcmp(name, "YPERF_RING_SIZE") == 0) {
        /* Read by yperf_init when each host thread allocates its
         * per-thread ring. Push to host env so the alloc picks the
         * caller's chosen capacity. Threads that have already
         * allocated keep their existing size — only NEW threads
         * (e.g. the fork'd child the wrapper is about to exec)
         * see the new value, which is exactly the per-app scope
         * we want. */
        setenv("YPERF_RING_SIZE", value, 1);
    }

    int idx = find_entry(&g_env, ctx, name);
    if (idx >= 0) {
        if (!overwrite) return 0;
        /* Replace the value — keep the name slot, reallocate value. */
        uint32_t new_val = copy_to_wasm(ctx, value);
        if (!new_val) return yos_errno_neg(ctx, ENOMEM);
        if (g_env.e[idx].value_off)
            yos_free(ctx, g_env.e[idx].value_off);
        g_env.e[idx].value_off = new_val;
        return 0;
    }

    if (g_env.count >= YOS_ENV_MAX)
        return yos_errno_neg(ctx, ENOMEM);
    uint32_t nm = copy_to_wasm(ctx, name);
    uint32_t vl = copy_to_wasm(ctx, value);
    if (!nm || !vl) {
        if (nm) yos_free(ctx, nm);
        if (vl) yos_free(ctx, vl);
        return yos_errno_neg(ctx, ENOMEM);
    }
    g_env.e[g_env.count].name_off  = nm;
    g_env.e[g_env.count].value_off = vl;
    g_env.e[g_env.count].name_len  = (uint32_t)strlen(name);
    g_env.count++;
    return 0;
}

int32_t yos_unsetenv(struct yos_exec_ctx *ctx, uint32_t name_off)
{
    env_init_once(ctx);
    if (!name_off || name_off >= ctx->memory_size)
        return yos_errno_neg(ctx, EINVAL);
    const char *name = (const char *)(ctx->memory + name_off);
    if (!*name || strchr(name, '=') != NULL)
        return yos_errno_neg(ctx, EINVAL);
    int idx = find_entry(&g_env, ctx, name);
    if (idx < 0) return 0;
    if (g_env.e[idx].name_off)  yos_free(ctx, g_env.e[idx].name_off);
    if (g_env.e[idx].value_off) yos_free(ctx, g_env.e[idx].value_off);
    /* Compact: copy last entry into this slot. */
    g_env.e[idx] = g_env.e[--g_env.count];
    g_env.e[g_env.count].name_off  = 0;
    g_env.e[g_env.count].value_off = 0;
    g_env.e[g_env.count].name_len  = 0;
    return 0;
}

int32_t yos_clearenv(struct yos_exec_ctx *ctx)
{
    env_init_once(ctx);
    for (int i = 0; i < g_env.count; i++) {
        if (g_env.e[i].name_off)  yos_free(ctx, g_env.e[i].name_off);
        if (g_env.e[i].value_off) yos_free(ctx, g_env.e[i].value_off);
        g_env.e[i].name_off  = 0;
        g_env.e[i].value_off = 0;
        g_env.e[i].name_len  = 0;
    }
    g_env.count = 0;
    return 0;
}

/* putenv("NAME=VALUE") — writes the input string into the env table.
 * POSIX warns the caller that putenv'd strings become part of the
 * environment (no copy); we copy because the wasm-side buffer
 * lifetime is unpredictable. */
int32_t yos_putenv(struct yos_exec_ctx *ctx, uint32_t s_off)
{
    env_init_once(ctx);
    if (!s_off || s_off >= ctx->memory_size)
        return yos_errno_neg(ctx, EINVAL);
    const char *s = (const char *)(ctx->memory + s_off);
    const char *eq = strchr(s, '=');
    if (!eq || eq == s)
        return yos_errno_neg(ctx, EINVAL);

    /* Re-do setenv with split name/value. */
    size_t nlen = (size_t)(eq - s);
    char nbuf[256];
    if (nlen >= sizeof nbuf)
        return yos_errno_neg(ctx, EINVAL);
    memcpy(nbuf, s, nlen);
    nbuf[nlen] = 0;
    /* Allocate a wasm-side copy of the name and the value to feed
     * setenv via the same offset path. */
    uint32_t name_off = copy_to_wasm(ctx, nbuf);
    uint32_t val_off  = copy_to_wasm(ctx, eq + 1);
    if (!name_off || !val_off) {
        if (name_off) yos_free(ctx, name_off);
        if (val_off)  yos_free(ctx, val_off);
        return yos_errno_neg(ctx, ENOMEM);
    }
    int32_t r = yos_setenv(ctx, name_off, val_off, 1);
    /* setenv copied the strings — free our temp duplicates. */
    yos_free(ctx, name_off);
    yos_free(ctx, val_off);
    return r;
}
