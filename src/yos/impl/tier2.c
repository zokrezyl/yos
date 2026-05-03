/* Tier 2 — sidecar wasm runtime hosting FreeBSD libc fns. See
 * impl/tier2.h for the design. */

#include "impl/tier2.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "wasm3.h"

/* Singletons. The sidecar is shared across all guest forks — Tier 2
 * fns are required to be pure (no per-ctx state). The init function
 * is called once from main.c at startup. */
static IM3Environment g_t2_env;
static IM3Runtime     g_t2_rt;
static int            g_t2_ok;  /* 1 if init succeeded */

static uint8_t *slurp(const char *path, size_t *out_size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return NULL; }
    uint8_t *buf = malloc((size_t)st.st_size);
    if (!buf) { close(fd); return NULL; }
    ssize_t n = read(fd, buf, (size_t)st.st_size);
    close(fd);
    if (n != st.st_size) { free(buf); return NULL; }
    *out_size = (size_t)st.st_size;
    return buf;
}

int yos_tier2_init(const char *libc_pure_path)
{
    g_t2_ok = 0;

    g_t2_env = m3_NewEnvironment();
    if (!g_t2_env) {
        fprintf(stderr, "yos: tier2: m3_NewEnvironment failed\n");
        return -1;
    }

    /* Stack size 64 KiB — same as guest's. Tier 2 fns are leaf-y;
     * 64 KiB is generous. No userdata: the sidecar runtime doesn't
     * need a yos_exec_ctx because Tier 2 is shared across procs. */
    g_t2_rt = m3_NewRuntime(g_t2_env, 64 * 1024, NULL);
    if (!g_t2_rt) {
        fprintf(stderr, "yos: tier2: m3_NewRuntime failed\n");
        m3_FreeEnvironment(g_t2_env);
        g_t2_env = NULL;
        return -1;
    }

    size_t  bytes_size = 0;
    uint8_t *bytes = slurp(libc_pure_path, &bytes_size);
    if (!bytes) {
        fprintf(stderr, "yos: tier2: cannot read %s: %s\n",
                libc_pure_path, strerror(errno));
        m3_FreeRuntime(g_t2_rt);
        m3_FreeEnvironment(g_t2_env);
        g_t2_rt = NULL; g_t2_env = NULL;
        return -1;
    }

    IM3Module mod = NULL;
    M3Result r = m3_ParseModule(g_t2_env, &mod, bytes, bytes_size);
    if (r) {
        fprintf(stderr, "yos: tier2: m3_ParseModule: %s\n", r);
        free(bytes);
        m3_FreeRuntime(g_t2_rt);
        m3_FreeEnvironment(g_t2_env);
        g_t2_rt = NULL; g_t2_env = NULL;
        return -1;
    }

    r = m3_LoadModule(g_t2_rt, mod);
    if (r) {
        fprintf(stderr, "yos: tier2: m3_LoadModule: %s\n", r);
        free(bytes);
        m3_FreeRuntime(g_t2_rt);
        m3_FreeEnvironment(g_t2_env);
        g_t2_rt = NULL; g_t2_env = NULL;
        return -1;
    }

    /* libc-pure.wasm doesn't import anything yet (the demo function
     * is self-contained). Once we add Tier-2 fns that need lower-
     * level libc (e.g. printf calling write), the matching env
     * imports will be linked here, routing back through the host
     * yos's own bridges. That binding work is deferred until the
     * first such function lands. */

    /* Note: bytes is owned by the runtime now; do NOT free. */
    g_t2_ok = 1;
    return 0;
}

IM3Function yos_tier2_find(const char *name)
{
    if (!g_t2_ok) return NULL;
    IM3Function f = NULL;
    M3Result r = m3_FindFunction(&f, g_t2_rt, name);
    if (r || !f) return NULL;
    return f;
}

IM3Function yos_tier2_resolve_once(IM3Function *cache, const char *name)
{
    if (*cache) return *cache;
    *cache = yos_tier2_find(name);
    return *cache;
}
