/* impl/libc/libarchive.c — host-side bridges exposing host libarchive
 * to the wasm guest as env.archive_* imports.
 *
 * Architecture mirrors liblua/openssl: the yos host binary links host
 * libarchive, the wasm guest (bsdtar, or a small libarchive driver)
 * carries no libarchive bodies, and every archive_* call resolves to
 * env.<name> at module load.
 *
 * Why this is a clean Tier-1 case (see the libbridge globals analysis):
 * libarchive exports ZERO writable file-scope globals. All state lives
 * in the caller-owned `struct archive *` and `struct archive_entry *`.
 * Per-guest isolation is therefore automatic — each guest holds its own
 * handles; the bridge just maps the i32 handle to the host pointer via
 * ctx->arc_handles[]. No global-swap dance.
 *
 * Scope of this first cut — the read/list/extract-data path:
 *   archive_read_new / _support_format_all / _support_filter_all /
 *   _open_memory / _next_header / _data / _data_skip / _free,
 *   archive_entry_pathname / _size / _filetype,
 *   archive_error_string / archive_errno.
 * That is enough to open an archive the guest holds in linear memory,
 * walk its entries, and pull out each entry's bytes.
 *
 * Deferred (next increments):
 *   - File/fd-backed open (archive_read_open_fd / _filename). The
 *     correct shape routes I/O through yos's vfs via a host-side read
 *     callback keyed on a guest fd, so host libarchive never touches a
 *     guest fd number directly. _open_memory needs none of that.
 *   - The write/disk-extraction surface (archive_write_disk_*).
 *   - Custom client callbacks (archive_read_open2) — same host->guest
 *     trampoline problem liblua defers for lua_CFunction.
 *
 * Conventions match the sibling bridges:
 *   - m3 raw-function args at _sp[1..] for "i(...)"/"I(...)" sigs
 *     (result in _sp[0]); at _sp[0..] for "v(...)".
 *   - Opaque pointers wrapped as i32 handles in ctx->arc_handles[].
 *   - const char * returns copied into a per-ctx scratch at the tail
 *     of guest memory; the guest must consume the offset before the
 *     next string-returning bridge call.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "wasm3.h"
#include "m3_env.h"
#include "yos/types.h"
#include <yos/ytrace/ytrace.h>

/* ── host libarchive forward decls ───────────────────────────────────
 * Declared here rather than via <archive.h> so the host build needs
 * only the shared library (-larchive), not libarchive's -dev headers
 * — same approach liblua.c takes with <lua.h>. Signatures pinned to
 * libarchive 3.x. la_int64_t/la_ssize_t are int64 on every host. */
struct archive;
struct archive_entry;
extern struct archive *archive_read_new(void);
extern int   archive_read_support_format_all(struct archive *);
extern int   archive_read_support_format_empty(struct archive *);
extern int   archive_read_support_format_raw(struct archive *);
extern int   archive_read_support_filter_all(struct archive *);
extern int   archive_read_open_memory(struct archive *, const void *, size_t);
extern int   archive_read_open_filename(struct archive *, const char *, size_t);
extern int   archive_read_close(struct archive *);
extern int64_t archive_read_data_into_fd(struct archive *, int);
extern const char *archive_version_details(void);
extern int   archive_read_next_header(struct archive *, struct archive_entry **);
extern int64_t archive_read_data(struct archive *, void *, size_t);
extern int   archive_read_data_skip(struct archive *);
extern int   archive_read_free(struct archive *);
extern const char *archive_error_string(struct archive *);
extern int   archive_errno(struct archive *);
extern const char *archive_entry_pathname(struct archive_entry *);
extern int64_t archive_entry_size(struct archive_entry *);
extern int   archive_entry_filetype(struct archive_entry *);

#define CTX(rt) ((struct yos_exec_ctx *)m3_GetUserData(rt))

#define YOS_ARC_HANDLES_INIT 16
#define YOS_ARC_HANDLES_GROW 16

/* ── handle table (struct archive * and struct archive_entry *) ─────── */

static int arc_handles_reserve(struct yos_exec_ctx *ctx)
{
    if (ctx->arc_handles_cap == 0) {
        size_t cap = YOS_ARC_HANDLES_INIT;
        void **slots = calloc(cap, sizeof(void *));
        if (!slots) return -1;
        ctx->arc_handles = slots;
        ctx->arc_handles_cap = (uint32_t)cap;
    }
    for (uint32_t i = 1; i < ctx->arc_handles_cap; ++i)
        if (!ctx->arc_handles[i]) return 0;
    size_t newcap = (size_t)ctx->arc_handles_cap + YOS_ARC_HANDLES_GROW;
    void **next = realloc(ctx->arc_handles, newcap * sizeof(void *));
    if (!next) return -1;
    memset(next + ctx->arc_handles_cap, 0,
           (newcap - ctx->arc_handles_cap) * sizeof(void *));
    ctx->arc_handles = next;
    ctx->arc_handles_cap = (uint32_t)newcap;
    return 0;
}

static uint32_t arc_handles_wrap(struct yos_exec_ctx *ctx, void *p)
{
    if (!p) return 0;
    if (arc_handles_reserve(ctx) < 0) return 0;
    for (uint32_t i = 1; i < ctx->arc_handles_cap; ++i)
        if (!ctx->arc_handles[i]) { ctx->arc_handles[i] = p; return i; }
    return 0;
}

/* Wrap p, reusing an existing handle if p is already in the table.
 * archive_read_next_header reuses one internal archive_entry across
 * calls, so without this the table would grow one slot per entry. */
static uint32_t arc_handles_wrap_unique(struct yos_exec_ctx *ctx, void *p)
{
    if (!p) return 0;
    for (uint32_t i = 1; i < ctx->arc_handles_cap; ++i)
        if (ctx->arc_handles[i] == p) return i;
    return arc_handles_wrap(ctx, p);
}

static void *arc_handles_resolve(struct yos_exec_ctx *ctx, uint32_t h)
{
    if (!ctx || !ctx->arc_handles || h == 0 || h >= ctx->arc_handles_cap)
        return NULL;
    return ctx->arc_handles[h];
}

static void *arc_handles_release(struct yos_exec_ctx *ctx, uint32_t h)
{
    if (!ctx || !ctx->arc_handles || h == 0 || h >= ctx->arc_handles_cap)
        return NULL;
    void *p = ctx->arc_handles[h];
    ctx->arc_handles[h] = NULL;
    return p;
}

/* ── guest-memory helpers ───────────────────────────────────────────── */

static void *guest_buf_rw(struct yos_exec_ctx *ctx, uint32_t off, size_t len)
{
    if (!ctx || !ctx->memory || off == 0) return NULL;
    if (off >= ctx->memory_size) return NULL;
    if (len > ctx->memory_size || off + len > ctx->memory_size) return NULL;
    return ctx->memory + off;
}

static const void *guest_buf_ro(struct yos_exec_ctx *ctx, uint32_t off,
                                size_t len)
{
    return guest_buf_rw(ctx, off, len);
}

/* Resolve a guest NUL-terminated string to a host pointer into guest
 * memory, verifying the NUL falls within bounds. Returns NULL for a
 * 0 offset (the caller maps that to "no filename" / stdin). */
static const char *guest_str(struct yos_exec_ctx *ctx, uint32_t off)
{
    if (!ctx || !ctx->memory || off == 0 || off >= ctx->memory_size)
        return NULL;
    for (uint32_t i = off; i < ctx->memory_size; i++)
        if (ctx->memory[i] == 0)
            return (const char *)(ctx->memory + off);
    return NULL;
}

/* Stash a host string into a fixed scratch slot at the tail of guest
 * memory and return its offset. The guest must consume the offset
 * before the next string-returning bridge call (same caveat as the
 * liblua/openssl string-return path). */
static uint32_t guest_stash_string(struct yos_exec_ctx *ctx, const char *s)
{
    if (!s || !ctx || !ctx->memory || ctx->memory_size < 4096 + 16)
        return 0;
    size_t len = strlen(s);
    if (len > 4095) len = 4095;
    uint32_t at = ctx->memory_size - 4096;
    memcpy(ctx->memory + at, s, len);
    ctx->memory[at + len] = 0;
    return at;
}

/* ── bridges ─────────────────────────────────────────────────────────
 * Reusable shorthands: A(slot) resolves an archive handle, E(slot) an
 * entry handle (same table). For "i(...)"/"I(...)" sigs args begin at
 * _sp[1]; the result is written to _sp[0]. */
#define A(slot) ((struct archive *)arc_handles_resolve(CTX(rt), (uint32_t)_sp[slot]))
#define E(slot) ((struct archive_entry *)arc_handles_resolve(CTX(rt), (uint32_t)_sp[slot]))

/* env.archive_read_new — i(). Returns an i32 handle (0 on failure). */
static const void *m3_yos_archive_read_new(IM3Runtime rt, IM3ImportContext _c,
                                           uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    struct archive *a = archive_read_new();
    _sp[0] = (uint64_t)arc_handles_wrap(CTX(rt), a);
    return NULL;
}

/* env.archive_read_support_format_all — i(a). */
static const void *m3_yos_archive_read_support_format_all(IM3Runtime rt,
        IM3ImportContext _c, uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    struct archive *a = A(1);
    _sp[0] = (uint64_t)(uint32_t)(a ? archive_read_support_format_all(a) : -1);
    return NULL;
}

/* env.archive_read_support_format_empty — i(a). */
static const void *m3_yos_archive_read_support_format_empty(IM3Runtime rt,
        IM3ImportContext _c, uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    struct archive *a = A(1);
    _sp[0] = (uint64_t)(uint32_t)(a ? archive_read_support_format_empty(a) : -1);
    return NULL;
}

/* env.archive_read_support_format_raw — i(a). */
static const void *m3_yos_archive_read_support_format_raw(IM3Runtime rt,
        IM3ImportContext _c, uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    struct archive *a = A(1);
    _sp[0] = (uint64_t)(uint32_t)(a ? archive_read_support_format_raw(a) : -1);
    return NULL;
}

/* env.archive_read_support_filter_all — i(a). */
static const void *m3_yos_archive_read_support_filter_all(IM3Runtime rt,
        IM3ImportContext _c, uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    struct archive *a = A(1);
    _sp[0] = (uint64_t)(uint32_t)(a ? archive_read_support_filter_all(a) : -1);
    return NULL;
}

/* env.archive_read_open_filename — i(a, filename_off, block_size).
 * A 0 offset means NULL = read stdin. The host opens the path through
 * host libc; for yos's passthrough filesystem that is the same file
 * the guest sees. (fd-table/vfs-routed open via a read callback is a
 * later refinement; the passthrough path is correct for real files.) */
static const void *m3_yos_archive_read_open_filename(IM3Runtime rt,
        IM3ImportContext _c, uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    struct yos_exec_ctx *ctx = CTX(rt);
    struct archive *a = A(1);
    uint32_t fn_off = (uint32_t)_sp[2];
    size_t block = (size_t)(uint32_t)_sp[3];
    const char *fn = fn_off ? guest_str(ctx, fn_off) : NULL;
    if (!a) { _sp[0] = (uint64_t)(uint32_t)-1; return NULL; }
    _sp[0] = (uint64_t)(uint32_t)archive_read_open_filename(a, fn, block);
    return NULL;
}

/* env.archive_read_close — i(a). */
static const void *m3_yos_archive_read_close(IM3Runtime rt,
        IM3ImportContext _c, uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    struct archive *a = A(1);
    _sp[0] = (uint64_t)(uint32_t)(a ? archive_read_close(a) : -1);
    return NULL;
}

/* env.archive_read_data_into_fd — I(a, fd). Streams the current
 * entry's data straight to a (guest == host, for std streams) fd.
 * Returns ARCHIVE_OK/_FATAL etc. as an int64. */
static const void *m3_yos_archive_read_data_into_fd(IM3Runtime rt,
        IM3ImportContext _c, uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    struct archive *a = A(1);
    int fd = (int)_sp[2];
    _sp[0] = (uint64_t)(a ? archive_read_data_into_fd(a, fd) : -1);
    return NULL;
}

/* env.archive_version_details — i(). Returns a stashed-string offset. */
static const void *m3_yos_archive_version_details(IM3Runtime rt,
        IM3ImportContext _c, uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    const char *s = archive_version_details();
    _sp[0] = (uint64_t)(s ? guest_stash_string(CTX(rt), s) : 0);
    return NULL;
}

/* env.archive_read_open_memory — i(a, buf_off, size).
 * libarchive references the buffer for the archive's lifetime; the
 * guest's linear memory is stable for the guest's lifetime, so handing
 * ctx->memory + off straight in is safe. */
static const void *m3_yos_archive_read_open_memory(IM3Runtime rt,
        IM3ImportContext _c, uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    struct yos_exec_ctx *ctx = CTX(rt);
    struct archive *a = A(1);
    uint32_t buf_off = (uint32_t)_sp[2];
    size_t size = (size_t)(uint32_t)_sp[3];
    const void *buf = guest_buf_ro(ctx, buf_off, size);
    if (!a || !buf) { _sp[0] = (uint64_t)(uint32_t)-1; return NULL; }
    _sp[0] = (uint64_t)(uint32_t)archive_read_open_memory(a, buf, size);
    return NULL;
}

/* env.archive_read_next_header — i(a, entry_slot_off).
 * Writes the entry's i32 handle into the guest's *entry slot and
 * returns the libarchive status (ARCHIVE_OK=0, ARCHIVE_EOF=1, ...). */
static const void *m3_yos_archive_read_next_header(IM3Runtime rt,
        IM3ImportContext _c, uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    struct yos_exec_ctx *ctx = CTX(rt);
    struct archive *a = A(1);
    uint32_t slot_off = (uint32_t)_sp[2];
    if (!a) { _sp[0] = (uint64_t)(uint32_t)-1; return NULL; }
    struct archive_entry *e = NULL;
    int r = archive_read_next_header(a, &e);
    if (r == 0 && e) {
        uint32_t h = arc_handles_wrap_unique(ctx, e);
        uint32_t *slot = (uint32_t *)guest_buf_rw(ctx, slot_off,
                                                  sizeof(uint32_t));
        if (slot) *slot = h;
    }
    _sp[0] = (uint64_t)(uint32_t)r;
    return NULL;
}

/* env.archive_read_data — I(a, buf_off, size). Returns bytes read
 * (>=0), 0 at end of entry, or a negative libarchive error code. */
static const void *m3_yos_archive_read_data(IM3Runtime rt,
        IM3ImportContext _c, uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    struct yos_exec_ctx *ctx = CTX(rt);
    struct archive *a = A(1);
    uint32_t buf_off = (uint32_t)_sp[2];
    size_t size = (size_t)(uint32_t)_sp[3];
    void *buf = guest_buf_rw(ctx, buf_off, size);
    if (!a || !buf) { _sp[0] = (uint64_t)(int64_t)-1; return NULL; }
    _sp[0] = (uint64_t)archive_read_data(a, buf, size);
    return NULL;
}

/* env.archive_read_data_skip — i(a). */
static const void *m3_yos_archive_read_data_skip(IM3Runtime rt,
        IM3ImportContext _c, uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    struct archive *a = A(1);
    _sp[0] = (uint64_t)(uint32_t)(a ? archive_read_data_skip(a) : -1);
    return NULL;
}

/* env.archive_read_free — i(a). Releases the handle too. */
static const void *m3_yos_archive_read_free(IM3Runtime rt,
        IM3ImportContext _c, uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    struct yos_exec_ctx *ctx = CTX(rt);
    struct archive *a = (struct archive *)arc_handles_release(ctx,
                                                       (uint32_t)_sp[1]);
    _sp[0] = (uint64_t)(uint32_t)(a ? archive_read_free(a) : 0);
    return NULL;
}

/* env.archive_error_string — i(a). Returns a stashed-string offset
 * (0 if none). */
static const void *m3_yos_archive_error_string(IM3Runtime rt,
        IM3ImportContext _c, uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    struct yos_exec_ctx *ctx = CTX(rt);
    struct archive *a = A(1);
    const char *s = a ? archive_error_string(a) : NULL;
    _sp[0] = (uint64_t)(s ? guest_stash_string(ctx, s) : 0);
    return NULL;
}

/* env.archive_errno — i(a). */
static const void *m3_yos_archive_errno(IM3Runtime rt,
        IM3ImportContext _c, uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    struct archive *a = A(1);
    _sp[0] = (uint64_t)(uint32_t)(a ? archive_errno(a) : 0);
    return NULL;
}

/* env.archive_entry_pathname — i(e). Returns a stashed-string offset. */
static const void *m3_yos_archive_entry_pathname(IM3Runtime rt,
        IM3ImportContext _c, uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    struct yos_exec_ctx *ctx = CTX(rt);
    struct archive_entry *e = E(1);
    const char *s = e ? archive_entry_pathname(e) : NULL;
    _sp[0] = (uint64_t)(s ? guest_stash_string(ctx, s) : 0);
    return NULL;
}

/* env.archive_entry_size — I(e). */
static const void *m3_yos_archive_entry_size(IM3Runtime rt,
        IM3ImportContext _c, uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    struct archive_entry *e = E(1);
    _sp[0] = (uint64_t)(e ? archive_entry_size(e) : 0);
    return NULL;
}

/* env.archive_entry_filetype — i(e). */
static const void *m3_yos_archive_entry_filetype(IM3Runtime rt,
        IM3ImportContext _c, uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    struct archive_entry *e = E(1);
    _sp[0] = (uint64_t)(uint32_t)(e ? archive_entry_filetype(e) : 0);
    return NULL;
}

/* ── teardown ───────────────────────────────────────────────────────
 * Free any archive handles the guest leaked (didn't archive_read_free)
 * and drop the table. archive_entry pointers are owned by their parent
 * archive, so freeing the archives is enough; we just NULL the slots. */
void yos_libarchive_ctx_free(struct yos_exec_ctx *ctx)
{
    if (!ctx || !ctx->arc_handles) return;
    free(ctx->arc_handles);
    ctx->arc_handles = NULL;
    ctx->arc_handles_cap = 0;
}

/* ── link ───────────────────────────────────────────────────────────── */

void yos_libarchive_link(IM3Module mod)
{
    m3_LinkRawFunction(mod, "env", "archive_read_new",            "i()",    m3_yos_archive_read_new);
    m3_LinkRawFunction(mod, "env", "archive_read_support_format_all", "i(i)", m3_yos_archive_read_support_format_all);
    m3_LinkRawFunction(mod, "env", "archive_read_support_format_empty", "i(i)", m3_yos_archive_read_support_format_empty);
    m3_LinkRawFunction(mod, "env", "archive_read_support_format_raw", "i(i)", m3_yos_archive_read_support_format_raw);
    m3_LinkRawFunction(mod, "env", "archive_read_support_filter_all", "i(i)", m3_yos_archive_read_support_filter_all);
    m3_LinkRawFunction(mod, "env", "archive_read_open_memory",    "i(iii)", m3_yos_archive_read_open_memory);
    m3_LinkRawFunction(mod, "env", "archive_read_open_filename",  "i(iii)", m3_yos_archive_read_open_filename);
    m3_LinkRawFunction(mod, "env", "archive_read_close",          "i(i)",   m3_yos_archive_read_close);
    m3_LinkRawFunction(mod, "env", "archive_read_data_into_fd",   "I(ii)",  m3_yos_archive_read_data_into_fd);
    m3_LinkRawFunction(mod, "env", "archive_version_details",     "i()",    m3_yos_archive_version_details);
    m3_LinkRawFunction(mod, "env", "archive_read_next_header",    "i(ii)",  m3_yos_archive_read_next_header);
    m3_LinkRawFunction(mod, "env", "archive_read_data",           "I(iii)", m3_yos_archive_read_data);
    m3_LinkRawFunction(mod, "env", "archive_read_data_skip",      "i(i)",   m3_yos_archive_read_data_skip);
    m3_LinkRawFunction(mod, "env", "archive_read_free",           "i(i)",   m3_yos_archive_read_free);
    m3_LinkRawFunction(mod, "env", "archive_error_string",        "i(i)",   m3_yos_archive_error_string);
    m3_LinkRawFunction(mod, "env", "archive_errno",               "i(i)",   m3_yos_archive_errno);
    m3_LinkRawFunction(mod, "env", "archive_entry_pathname",      "i(i)",   m3_yos_archive_entry_pathname);
    m3_LinkRawFunction(mod, "env", "archive_entry_size",          "I(i)",   m3_yos_archive_entry_size);
    m3_LinkRawFunction(mod, "env", "archive_entry_filetype",      "i(i)",   m3_yos_archive_entry_filetype);
}
