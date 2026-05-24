/* impl/libc/liblua.c — host-side bridges that expose host liblua-5.1
 * to the wasm guest as env.lua_* / env.luaL_* / env.luaopen_* imports.
 *
 * Architecture mirrors openssl: the yos host binary links against
 * host liblua-5.1, the wasm guest carries no Lua C bodies at link
 * time, every Lua C-API call resolves to env.<name> at module load.
 *
 * Why this is the cleanest possible Tier-1 case (see
 * build-tools/libbridge/policies/lua.yaml): liblua has ZERO mutable
 * file-scope globals. Every API function takes lua_State *L
 * explicitly; all per-instance state lives inside L. Per-guest
 * isolation = one luaL_newstate() per yos_exec_ctx, pass L through.
 * No subinterpreter swap dance like Python.
 *
 * The bridge surface here is roughly the public Lua 5.1 C API:
 * state mgmt, stack ops, value push/get, table ops, pcall + load,
 * registry, metatables, coroutines, gc, plus the luaL_* convenience
 * layer. ~80 functions; close to everything nvim / lua-using guests
 * need short of C-callback registration.
 *
 * Deferred (refused or stubbed for the first cut):
 *   - lua_pushcfunction / lua_pushcclosure
 *     The guest's "C function" is a wasm function reference. Calling
 *     it from host Lua requires a trampoline that dispatches back
 *     into the wasm runtime — not yet implemented. Bridges return 0
 *     and push nil instead. nvim's Lua-to-vim glue won't work until
 *     this is added.
 *   - lua_atpanic
 *     Panic = the wasm guest's problem. We don't intercept.
 *   - lua_setallocf / lua_getallocf
 *     yos doesn't expose its allocator to lua.
 *
 * Conventions matching the openssl bridge:
 *   - Args at sp[0..] for "v(...)" bridges, sp[1..] for "i(...)"
 *     (see comment above BR_RETPTR_NOARG in openssl.c for wasm3
 *     calling convention).
 *   - Opaque pointers wrapped as i32 handles via lua_handles[].
 *   - Strings: const char * args read NUL-terminated from guest
 *     memory; bounds-checked. Return strings copied into a small
 *     per-ctx string pool slot at the top of guest memory (same
 *     mechanism as OpenSSL_version). */

#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "wasm3.h"
#include "m3_env.h"
#include "yos/types.h"
#include <yos/ytrace/ytrace.h>

/* Lua 5.1 forward decls. Pin the layout of the things we need
 * without pulling <lua.h> into yos's other TUs. */
typedef struct lua_State lua_State;
typedef double lua_Number;
typedef ptrdiff_t lua_Integer;
typedef int (*lua_CFunction)(lua_State *L);
typedef const char *(*lua_Reader)(lua_State *L, void *ud, size_t *sz);
typedef int (*lua_Writer)(lua_State *L, const void *p, size_t sz, void *ud);

/* ── State ───────────────────────────────────────────────────────── */
extern lua_State *luaL_newstate(void);
extern void       lua_close(lua_State *L);
extern void       luaL_openlibs(lua_State *L);
extern lua_State *lua_newthread(lua_State *L);
extern int        lua_status(lua_State *L);
extern int        lua_resume(lua_State *L, int narg);
extern int        lua_yield(lua_State *L, int nresults);

/* ── Stack ──────────────────────────────────────────────────────── */
extern int  lua_gettop(lua_State *L);
extern void lua_settop(lua_State *L, int idx);
extern void lua_pushvalue(lua_State *L, int idx);
extern void lua_remove(lua_State *L, int idx);
extern void lua_insert(lua_State *L, int idx);
extern void lua_replace(lua_State *L, int idx);
extern int  lua_checkstack(lua_State *L, int extra);
extern void lua_xmove(lua_State *from, lua_State *to, int n);

/* ── Type queries ───────────────────────────────────────────────── */
extern int         lua_type(lua_State *L, int idx);
extern const char *lua_typename(lua_State *L, int tp);
extern int         lua_isnumber   (lua_State *L, int idx);
extern int         lua_isstring   (lua_State *L, int idx);
extern int         lua_iscfunction(lua_State *L, int idx);
extern int         lua_isuserdata (lua_State *L, int idx);
extern int         lua_rawequal   (lua_State *L, int i1, int i2);
extern int         lua_equal      (lua_State *L, int i1, int i2);
extern int         lua_lessthan   (lua_State *L, int i1, int i2);

/* ── Get value from stack ───────────────────────────────────────── */
extern lua_Number  lua_tonumber  (lua_State *L, int idx);
extern lua_Integer lua_tointeger (lua_State *L, int idx);
extern int         lua_toboolean (lua_State *L, int idx);
extern const char *lua_tolstring (lua_State *L, int idx, size_t *len);
extern size_t      lua_objlen    (lua_State *L, int idx);
extern lua_CFunction lua_tocfunction(lua_State *L, int idx);
extern void       *lua_touserdata(lua_State *L, int idx);
extern lua_State  *lua_tothread  (lua_State *L, int idx);
extern const void *lua_topointer (lua_State *L, int idx);

/* ── Push to stack ──────────────────────────────────────────────── */
extern void        lua_pushnil          (lua_State *L);
extern void        lua_pushnumber       (lua_State *L, lua_Number n);
extern void        lua_pushinteger      (lua_State *L, lua_Integer n);
extern void        lua_pushlstring      (lua_State *L, const char *s, size_t len);
extern void        lua_pushstring       (lua_State *L, const char *s);
extern void        lua_pushboolean      (lua_State *L, int b);
extern void        lua_pushlightuserdata(lua_State *L, void *p);
extern int         lua_pushthread       (lua_State *L);
extern void        lua_pushcclosure     (lua_State *L, lua_CFunction fn, int n);

/* ── Tables ─────────────────────────────────────────────────────── */
extern void  lua_createtable   (lua_State *L, int narr, int nrec);
extern void  lua_gettable      (lua_State *L, int idx);
extern void  lua_getfield      (lua_State *L, int idx, const char *k);
extern void  lua_rawget        (lua_State *L, int idx);
extern void  lua_rawgeti       (lua_State *L, int idx, int n);
extern void *lua_newuserdata   (lua_State *L, size_t sz);
extern int   lua_getmetatable  (lua_State *L, int objindex);
extern void  lua_getfenv       (lua_State *L, int idx);
extern void  lua_settable      (lua_State *L, int idx);
extern void  lua_setfield      (lua_State *L, int idx, const char *k);
extern void  lua_rawset        (lua_State *L, int idx);
extern void  lua_rawseti       (lua_State *L, int idx, int n);
extern int   lua_setmetatable  (lua_State *L, int objindex);
extern int   lua_setfenv       (lua_State *L, int idx);
extern int   lua_next          (lua_State *L, int idx);
extern void  lua_concat        (lua_State *L, int n);

/* ── Call / Load / Error ────────────────────────────────────────── */
extern void  lua_call (lua_State *L, int nargs, int nresults);
extern int   lua_pcall(lua_State *L, int nargs, int nresults, int errfunc);
extern int   lua_error(lua_State *L);
extern int   lua_load (lua_State *L, lua_Reader reader, void *data,
                       const char *chunkname);
extern int   lua_dump (lua_State *L, lua_Writer writer, void *data);

/* ── GC ─────────────────────────────────────────────────────────── */
extern int   lua_gc(lua_State *L, int what, int data);

/* ── luaL convenience ───────────────────────────────────────────── */
extern int   luaL_loadbuffer  (lua_State *L, const char *buf, size_t sz,
                               const char *name);
extern int   luaL_loadstring  (lua_State *L, const char *s);
extern int   luaL_loadfile    (lua_State *L, const char *filename);
extern int   luaL_ref         (lua_State *L, int t);
extern void  luaL_unref       (lua_State *L, int t, int ref);
extern int   luaL_newmetatable(lua_State *L, const char *tname);
extern void *luaL_checkudata  (lua_State *L, int ud, const char *tname);
extern int   luaL_getmetafield(lua_State *L, int obj, const char *e);
extern int   luaL_callmeta    (lua_State *L, int obj, const char *e);
extern lua_Number  luaL_checknumber (lua_State *L, int narg);
extern lua_Integer luaL_checkinteger(lua_State *L, int narg);
extern const char *luaL_checklstring(lua_State *L, int narg, size_t *l);
extern lua_Number  luaL_optnumber  (lua_State *L, int narg, lua_Number d);
extern lua_Integer luaL_optinteger (lua_State *L, int narg, lua_Integer d);
extern const char *luaL_optlstring (lua_State *L, int narg, const char *d, size_t *l);
extern void        luaL_checktype  (lua_State *L, int narg, int t);
extern void        luaL_checkany   (lua_State *L, int narg);
extern int         luaL_checkstack_real(lua_State *L, int sz, const char *msg);
#define luaL_checkstack luaL_checkstack_real  /* avoid colliding with macro form */
extern int         luaL_error      (lua_State *L, const char *fmt, ...);
extern int         luaL_argerror   (lua_State *L, int numarg, const char *extramsg);
extern int         luaL_typerror   (lua_State *L, int narg, const char *tname);
extern int         luaL_where      (lua_State *L, int lvl);

/* ── handle table (mirror ssl_handles_*) ───────────────────────── */

#define YOS_LUA_HANDLES_GROW 8

static int lua_handles_reserve(struct yos_exec_ctx *ctx)
{
    if (!ctx) return -1;
    if (ctx->lua_handles_cap == 0) {
        size_t cap = YOS_LUA_HANDLES_GROW;
        void **slots = calloc(cap, sizeof(void *));
        if (!slots) return -1;
        ctx->lua_handles = slots;
        ctx->lua_handles_cap = (uint32_t)cap;
        return 0;
    }
    for (uint32_t i = 1; i < ctx->lua_handles_cap; ++i)
        if (!ctx->lua_handles[i]) return 0;
    size_t newcap = (size_t)ctx->lua_handles_cap + YOS_LUA_HANDLES_GROW;
    void **next = realloc(ctx->lua_handles, newcap * sizeof(void *));
    if (!next) return -1;
    memset(next + ctx->lua_handles_cap, 0,
           (newcap - ctx->lua_handles_cap) * sizeof(void *));
    ctx->lua_handles = next;
    ctx->lua_handles_cap = (uint32_t)newcap;
    return 0;
}

static uint32_t lua_handles_wrap(struct yos_exec_ctx *ctx, void *p)
{
    if (!p) return 0;
    if (lua_handles_reserve(ctx) < 0) return 0;
    for (uint32_t i = 1; i < ctx->lua_handles_cap; ++i)
        if (!ctx->lua_handles[i]) { ctx->lua_handles[i] = p; return i; }
    return 0;
}

static void *lua_handles_resolve(struct yos_exec_ctx *ctx, uint32_t h)
{
    if (!ctx || !ctx->lua_handles || h == 0 || h >= ctx->lua_handles_cap)
        return NULL;
    return ctx->lua_handles[h];
}

static void *lua_handles_release(struct yos_exec_ctx *ctx, uint32_t h)
{
    if (!ctx || !ctx->lua_handles || h == 0 || h >= ctx->lua_handles_cap)
        return NULL;
    void *p = ctx->lua_handles[h];
    ctx->lua_handles[h] = NULL;
    return p;
}

#define CTX(rt) ((struct yos_exec_ctx *)m3_GetUserData(rt))

/* ── memory-safe accessors for wasm pointer args ───────────────── */

static const void *guest_buf_ro(struct yos_exec_ctx *ctx,
                                uint32_t off, uint32_t len)
{
    if (!ctx || !ctx->memory) return NULL;
    if (len == 0) return ctx->memory + off;
    uint64_t end = (uint64_t)off + (uint64_t)len;
    if (off >= ctx->memory_size || end > ctx->memory_size) return NULL;
    return ctx->memory + off;
}

static void *guest_buf_rw(struct yos_exec_ctx *ctx,
                          uint32_t off, uint32_t len)
{
    return (void *)guest_buf_ro(ctx, off, len);
}

static const char *guest_cstr(struct yos_exec_ctx *ctx, uint32_t off)
{
    if (!ctx || !ctx->memory || off == 0 || off >= ctx->memory_size)
        return NULL;
    const char *p = (const char *)(ctx->memory + off);
    const char *end = (const char *)(ctx->memory + ctx->memory_size);
    for (const char *q = p; q < end; ++q) if (*q == 0) return p;
    return NULL;
}

/* String return path: stash result in a small per-ctx scratch at the
 * tail of guest linear memory and return its offset. NOT thread-safe
 * across concurrent guest threads — same caveat as OpenSSL_version's
 * scratch. Good enough for the typical single-threaded Lua usage. */
static uint32_t guest_stash_string(struct yos_exec_ctx *ctx,
                                   const char *s, size_t len)
{
    if (!s || !ctx || !ctx->memory) return 0;
    /* Reserve the top 4 KiB of guest memory as a rolling scratch.
     * Successive stashes can overwrite each other; the wasm caller
     * is expected to copy the returned pointer before another bridge
     * call. */
    if (ctx->memory_size < 4096 + 16) return 0;
    static __thread uint32_t bump;
    static __thread uint32_t base;
    if (!base) base = ctx->memory_size - 4096;
    if (bump + len + 1 > 4096) bump = 0;
    uint32_t at = base + bump;
    memcpy(ctx->memory + at, s, len);
    ctx->memory[at + len] = 0;
    bump += (uint32_t)len + 1;
    return at;
}

/* Convenience: resolve L from sp[idx]. Caller decides idx based on
 * whether the bridge is "i(...)" (args start at sp[1]) or "v(...)"
 * (args start at sp[0]). */
#define L_AT(idx) ((lua_State *)lua_handles_resolve(CTX(rt), (uint32_t)_sp[idx]))

/* ── bridges ────────────────────────────────────────────────────── */

/* env.luaL_newstate — i32(). Returns a NEW lua_State handle. The
 * first call from a guest creates the main state; later calls would
 * create additional ones (rare; usually only one). */
static const void *m3_yos_luaL_newstate(IM3Runtime rt, IM3ImportContext _c,
                                        uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    struct yos_exec_ctx *ctx = CTX(rt);
    lua_State *L = luaL_newstate();
    _sp[0] = (uint64_t)lua_handles_wrap(ctx, L);
    ydebug("luaL_newstate() = handle %u (p=%p)\n", (uint32_t)_sp[0], (void *)L);
    return NULL;
}

/* env.lua_close — v(L_h). Release the handle, close the state. */
static const void *m3_yos_lua_close(IM3Runtime rt, IM3ImportContext _c,
                                    uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    lua_State *L = (lua_State *)lua_handles_release(CTX(rt), (uint32_t)_sp[0]);
    if (L) lua_close(L);
    return NULL;
}

/* env.luaL_openlibs — v(L_h) */
static const void *m3_yos_luaL_openlibs(IM3Runtime rt, IM3ImportContext _c,
                                        uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    lua_State *L = (lua_State *)lua_handles_resolve(CTX(rt), (uint32_t)_sp[0]);
    if (L) luaL_openlibs(L);
    return NULL;
}

/* env.lua_newthread — i32(L_h). Returns NEW state handle sharing
 * globals/registry with L. */
static const void *m3_yos_lua_newthread(IM3Runtime rt, IM3ImportContext _c,
                                        uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    struct yos_exec_ctx *ctx = CTX(rt);
    lua_State *L = (lua_State *)lua_handles_resolve(ctx, (uint32_t)_sp[1]);
    lua_State *co = L ? lua_newthread(L) : NULL;
    _sp[0] = (uint64_t)lua_handles_wrap(ctx, co);
    return NULL;
}

/* ── stack ops ──────────────────────────────────────────────────── */

/* env.lua_gettop — i32(L_h) */
static const void *m3_yos_lua_gettop(IM3Runtime rt, IM3ImportContext _c,
                                     uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    lua_State *L = L_AT(1);
    _sp[0] = (uint64_t)(uint32_t)(L ? lua_gettop(L) : 0);
    return NULL;
}

/* env.lua_settop — v(L_h, int idx) */
static const void *m3_yos_lua_settop(IM3Runtime rt, IM3ImportContext _c,
                                     uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    lua_State *L = (lua_State *)lua_handles_resolve(CTX(rt), (uint32_t)_sp[0]);
    int idx = (int)_sp[1];
    if (L) lua_settop(L, idx);
    return NULL;
}

/* Helper macro: void(L, int) */
#define BR_VOID_L_INT(NAME, FN)                                                \
static const void *m3_yos_##NAME(IM3Runtime rt, IM3ImportContext _c,           \
                                 uint64_t *_sp, void *_m)                      \
{ (void)_c; (void)_m;                                                          \
  lua_State *L = (lua_State *)lua_handles_resolve(CTX(rt), (uint32_t)_sp[0]);  \
  int x = (int)_sp[1];                                                          \
  if (L) FN(L, x);                                                              \
  return NULL; }

/* Helper macro: int(L, int) */
#define BR_INT_L_INT(NAME, FN)                                                 \
static const void *m3_yos_##NAME(IM3Runtime rt, IM3ImportContext _c,           \
                                 uint64_t *_sp, void *_m)                      \
{ (void)_c; (void)_m;                                                          \
  lua_State *L = (lua_State *)lua_handles_resolve(CTX(rt), (uint32_t)_sp[1]);  \
  int x = (int)_sp[2];                                                          \
  _sp[0] = (uint64_t)(uint32_t)(L ? FN(L, x) : 0);                              \
  return NULL; }

/* Helper macro: int(L, int, int) */
#define BR_INT_L_INT_INT(NAME, FN)                                             \
static const void *m3_yos_##NAME(IM3Runtime rt, IM3ImportContext _c,           \
                                 uint64_t *_sp, void *_m)                      \
{ (void)_c; (void)_m;                                                          \
  lua_State *L = (lua_State *)lua_handles_resolve(CTX(rt), (uint32_t)_sp[1]);  \
  int a = (int)_sp[2]; int b = (int)_sp[3];                                     \
  _sp[0] = (uint64_t)(uint32_t)(L ? FN(L, a, b) : 0);                           \
  return NULL; }

/* Helper macro: void(L, int, int) */
#define BR_VOID_L_INT_INT(NAME, FN)                                            \
static const void *m3_yos_##NAME(IM3Runtime rt, IM3ImportContext _c,           \
                                 uint64_t *_sp, void *_m)                      \
{ (void)_c; (void)_m;                                                          \
  lua_State *L = (lua_State *)lua_handles_resolve(CTX(rt), (uint32_t)_sp[0]);  \
  int a = (int)_sp[1]; int b = (int)_sp[2];                                     \
  if (L) FN(L, a, b);                                                           \
  return NULL; }

BR_VOID_L_INT (lua_pushvalue,   lua_pushvalue)
BR_VOID_L_INT (lua_remove,      lua_remove)
BR_VOID_L_INT (lua_insert,      lua_insert)
BR_VOID_L_INT (lua_replace,     lua_replace)
BR_INT_L_INT  (lua_checkstack,  lua_checkstack)
BR_INT_L_INT  (lua_type,        lua_type)
BR_INT_L_INT  (lua_isnumber,    lua_isnumber)
BR_INT_L_INT  (lua_isstring,    lua_isstring)
BR_INT_L_INT  (lua_iscfunction, lua_iscfunction)
BR_INT_L_INT  (lua_isuserdata,  lua_isuserdata)
BR_INT_L_INT_INT (lua_rawequal, lua_rawequal)
BR_INT_L_INT_INT (lua_equal,    lua_equal)
BR_INT_L_INT_INT (lua_lessthan, lua_lessthan)
BR_INT_L_INT  (lua_toboolean,   lua_toboolean)
BR_INT_L_INT  (lua_objlen,      lua_objlen)
BR_INT_L_INT  (lua_getmetatable,lua_getmetatable)
BR_INT_L_INT  (lua_setmetatable,lua_setmetatable)
BR_INT_L_INT  (lua_setfenv,     lua_setfenv)
BR_INT_L_INT  (lua_next,        lua_next)
/* lua_error takes only (L) — one arg. Hand-bridge. */
static const void *m3_yos_lua_error(IM3Runtime rt, IM3ImportContext _c,
                                    uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    lua_State *L = (lua_State *)lua_handles_resolve(CTX(rt), (uint32_t)_sp[1]);
    _sp[0] = (uint64_t)(uint32_t)(L ? lua_error(L) : -1);
    return NULL;
}
BR_VOID_L_INT (lua_concat,      lua_concat)
BR_VOID_L_INT (lua_getfenv,     lua_getfenv)
BR_VOID_L_INT (lua_gettable,    lua_gettable)
BR_VOID_L_INT (lua_settable,    lua_settable)
BR_VOID_L_INT (lua_rawget,      lua_rawget)
BR_VOID_L_INT (lua_rawset,      lua_rawset)
BR_VOID_L_INT_INT (lua_rawgeti, lua_rawgeti)
BR_VOID_L_INT_INT (lua_rawseti, lua_rawseti)
BR_VOID_L_INT_INT (lua_createtable, lua_createtable)

/* env.lua_xmove — v(from_h, to_h, int n) */
static const void *m3_yos_lua_xmove(IM3Runtime rt, IM3ImportContext _c,
                                    uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    struct yos_exec_ctx *ctx = CTX(rt);
    lua_State *from = (lua_State *)lua_handles_resolve(ctx, (uint32_t)_sp[0]);
    lua_State *to   = (lua_State *)lua_handles_resolve(ctx, (uint32_t)_sp[1]);
    int n = (int)_sp[2];
    if (from && to) lua_xmove(from, to, n);
    return NULL;
}

/* ── type queries ──────────────────────────────────────────────── */

/* env.lua_typename — i32(L_h, int tp). Returns offset of a stashed
 * copy of the name string. */
static const void *m3_yos_lua_typename(IM3Runtime rt, IM3ImportContext _c,
                                       uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    struct yos_exec_ctx *ctx = CTX(rt);
    lua_State *L = (lua_State *)lua_handles_resolve(ctx, (uint32_t)_sp[1]);
    int tp = (int)_sp[2];
    const char *s = L ? lua_typename(L, tp) : "?";
    _sp[0] = s ? (uint64_t)guest_stash_string(ctx, s, strlen(s)) : 0;
    return NULL;
}

/* ── get values ─────────────────────────────────────────────────── */

/* env.lua_tonumber — f64(L_h, int idx) */
static const void *m3_yos_lua_tonumber(IM3Runtime rt, IM3ImportContext _c,
                                       uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    lua_State *L = (lua_State *)lua_handles_resolve(CTX(rt), (uint32_t)_sp[1]);
    int idx = (int)_sp[2];
    double v = L ? (double)lua_tonumber(L, idx) : 0.0;
    memcpy(&_sp[0], &v, sizeof(double));
    return NULL;
}

/* env.lua_tointeger — i32(L_h, int idx).
 * lua_Integer is ptrdiff_t which is 32-bit on wasm32, 64-bit on
 * host. Narrow on the way back. */
static const void *m3_yos_lua_tointeger(IM3Runtime rt, IM3ImportContext _c,
                                        uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    lua_State *L = (lua_State *)lua_handles_resolve(CTX(rt), (uint32_t)_sp[1]);
    int idx = (int)_sp[2];
    _sp[0] = (uint64_t)(uint32_t)(L ? (int32_t)lua_tointeger(L, idx) : 0);
    return NULL;
}

/* env.lua_tolstring — i32(L_h, int idx, len_off).
 *
 * Host lua_tolstring returns a const char * into Lua's internal
 * string memory. The string is valid until the value is popped from
 * the stack. We copy into a small per-ctx scratch and hand back a
 * guest-memory offset. The guest is expected to copy the value
 * before it makes another bridge call. */
static const void *m3_yos_lua_tolstring(IM3Runtime rt, IM3ImportContext _c,
                                        uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    struct yos_exec_ctx *ctx = CTX(rt);
    lua_State *L = (lua_State *)lua_handles_resolve(ctx, (uint32_t)_sp[1]);
    int idx = (int)_sp[2];
    uint32_t len_off = (uint32_t)_sp[3];
    if (!L) { _sp[0] = 0; return NULL; }
    size_t len = 0;
    const char *s = lua_tolstring(L, idx, &len);
    if (!s) { _sp[0] = 0; return NULL; }
    if (len_off) {
        uint32_t *lp = (uint32_t *)guest_buf_rw(ctx, len_off, sizeof(uint32_t));
        if (lp) *lp = (uint32_t)len;
    }
    _sp[0] = (uint64_t)guest_stash_string(ctx, s, len);
    return NULL;
}

/* env.lua_tocfunction — i32(L_h, int idx). Returns 0 (NULL); we
 * don't bridge C function callbacks yet. */
static const void *m3_yos_lua_tocfunction(IM3Runtime rt, IM3ImportContext _c,
                                          uint64_t *_sp, void *_m)
{
    (void)rt; (void)_c; (void)_m; (void)_sp;
    _sp[0] = 0;
    return NULL;
}

/* env.lua_touserdata — i32(L_h, int idx).
 * Returns a wasm-side i32 representation of a host void *. We don't
 * have a clean way to expose a host pointer to the guest; the
 * common pattern is for the guest to use lua_newuserdata which we
 * DO bridge with a wasm-offset return. For arbitrary userdata
 * created by other means, return 0. */
static const void *m3_yos_lua_touserdata(IM3Runtime rt, IM3ImportContext _c,
                                         uint64_t *_sp, void *_m)
{
    (void)rt; (void)_c; (void)_m;
    /* Stub — see note above. Caller's lua_newuserdata returns a
     * guest-memory offset; if they pass that back here, returning
     * the same offset would be ideal but requires tracking which
     * offsets are userdata. First cut: 0. */
    _sp[0] = 0;
    return NULL;
}

/* env.lua_tothread — i32(L_h, int idx). Wrap result as lua handle. */
static const void *m3_yos_lua_tothread(IM3Runtime rt, IM3ImportContext _c,
                                       uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    struct yos_exec_ctx *ctx = CTX(rt);
    lua_State *L = (lua_State *)lua_handles_resolve(ctx, (uint32_t)_sp[1]);
    int idx = (int)_sp[2];
    lua_State *co = L ? lua_tothread(L, idx) : NULL;
    /* If the thread is already in our handle table, return existing
     * id; otherwise wrap fresh. Linear scan for now. */
    uint32_t found = 0;
    if (co && ctx->lua_handles) {
        for (uint32_t i = 1; i < ctx->lua_handles_cap; ++i)
            if (ctx->lua_handles[i] == co) { found = i; break; }
    }
    _sp[0] = (uint64_t)(found ? found : (co ? lua_handles_wrap(ctx, co) : 0));
    return NULL;
}

/* env.lua_topointer — i32(L_h, int idx). Returns an opaque
 * identity-comparable token. Host pointer can't be exposed
 * directly; use a stable hash for identity comparison. */
static const void *m3_yos_lua_topointer(IM3Runtime rt, IM3ImportContext _c,
                                        uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    lua_State *L = (lua_State *)lua_handles_resolve(CTX(rt), (uint32_t)_sp[1]);
    int idx = (int)_sp[2];
    const void *p = L ? lua_topointer(L, idx) : NULL;
    /* Cast the host pointer to a 32-bit token. Collisions possible
     * but the guest only uses this for identity comparison and the
     * pointer-value high bits are essentially random. */
    _sp[0] = (uint64_t)(uint32_t)(uintptr_t)p;
    return NULL;
}

/* ── push values ────────────────────────────────────────────────── */

/* env.lua_pushnil — v(L_h) */
static const void *m3_yos_lua_pushnil(IM3Runtime rt, IM3ImportContext _c,
                                      uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    lua_State *L = (lua_State *)lua_handles_resolve(CTX(rt), (uint32_t)_sp[0]);
    if (L) lua_pushnil(L);
    return NULL;
}

/* env.lua_pushnumber — v(L_h, f64 n) */
static const void *m3_yos_lua_pushnumber(IM3Runtime rt, IM3ImportContext _c,
                                         uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    lua_State *L = (lua_State *)lua_handles_resolve(CTX(rt), (uint32_t)_sp[0]);
    double n; memcpy(&n, &_sp[1], sizeof(double));
    if (L) lua_pushnumber(L, (lua_Number)n);
    return NULL;
}

/* env.lua_pushinteger — v(L_h, int n) */
BR_VOID_L_INT(lua_pushinteger, lua_pushinteger)
BR_VOID_L_INT(lua_pushboolean, lua_pushboolean)

/* env.lua_pushlstring — v(L_h, str_off, size_t len) */
static const void *m3_yos_lua_pushlstring(IM3Runtime rt, IM3ImportContext _c,
                                          uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    struct yos_exec_ctx *ctx = CTX(rt);
    lua_State *L = (lua_State *)lua_handles_resolve(ctx, (uint32_t)_sp[0]);
    uint32_t off = (uint32_t)_sp[1];
    uint32_t len = (uint32_t)_sp[2];
    const char *s = len ? (const char *)guest_buf_ro(ctx, off, len) : "";
    if (L && (s || len == 0)) lua_pushlstring(L, s, len);
    return NULL;
}

/* env.lua_pushstring — v(L_h, str_off) */
static const void *m3_yos_lua_pushstring(IM3Runtime rt, IM3ImportContext _c,
                                         uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    struct yos_exec_ctx *ctx = CTX(rt);
    lua_State *L = (lua_State *)lua_handles_resolve(ctx, (uint32_t)_sp[0]);
    uint32_t off = (uint32_t)_sp[1];
    if (!L) return NULL;
    if (off == 0) { lua_pushnil(L); return NULL; }
    const char *s = guest_cstr(ctx, off);
    if (s) lua_pushstring(L, s); else lua_pushnil(L);
    return NULL;
}

/* env.lua_pushlightuserdata — v(L_h, p_off).
 * The wasm guest passes a wasm offset. Map to a host address inside
 * its own linear memory. The guest expects identity-stable
 * semantics — two pushlightuserdata calls with the same offset
 * produce equal Lua values. (ctx->memory + off) gives that. */
static const void *m3_yos_lua_pushlightuserdata(IM3Runtime rt, IM3ImportContext _c,
                                                uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    struct yos_exec_ctx *ctx = CTX(rt);
    lua_State *L = (lua_State *)lua_handles_resolve(ctx, (uint32_t)_sp[0]);
    uint32_t off = (uint32_t)_sp[1];
    if (L) lua_pushlightuserdata(L, ctx->memory + off);
    return NULL;
}

/* env.lua_pushthread — i32(L_h). Pushes L itself as a thread. */
static const void *m3_yos_lua_pushthread(IM3Runtime rt, IM3ImportContext _c,
                                         uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    lua_State *L = (lua_State *)lua_handles_resolve(CTX(rt), (uint32_t)_sp[1]);
    _sp[0] = (uint64_t)(uint32_t)(L ? lua_pushthread(L) : 0);
    return NULL;
}

/* env.lua_pushcclosure — v(L_h, fn_idx, int n).
 *
 * DEFERRED: registering a wasm function as a Lua C closure requires
 * a host-side trampoline that dispatches calls back into the wasm
 * runtime. Not implemented in this first cut. Push nil and discard
 * the n upvalues from the stack so the caller's state stays
 * coherent. The Lua side will see nil instead of the function — any
 * call attempt will raise "attempt to call a nil value".
 *
 * (Once the trampoline lands, the bridge will wrap fn_idx + n
 * upvalues into a host-side stub that calls back into wasm.) */
static const void *m3_yos_lua_pushcclosure(IM3Runtime rt, IM3ImportContext _c,
                                           uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    lua_State *L = (lua_State *)lua_handles_resolve(CTX(rt), (uint32_t)_sp[0]);
    int n = (int)_sp[2];
    if (!L) return NULL;
    /* Pop the n upvalues the caller pushed, then push nil so the
     * stack-balance contract holds. */
    lua_settop(L, lua_gettop(L) - n);
    lua_pushnil(L);
    ywarn("lua_pushcclosure: C function callback not bridged yet; "
          "pushing nil\n");
    return NULL;
}

/* ── tables with const char *k arg ──────────────────────────────── */

/* env.lua_getfield — v(L_h, int idx, k_off) */
static const void *m3_yos_lua_getfield(IM3Runtime rt, IM3ImportContext _c,
                                       uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    struct yos_exec_ctx *ctx = CTX(rt);
    lua_State *L = (lua_State *)lua_handles_resolve(ctx, (uint32_t)_sp[0]);
    int idx = (int)_sp[1];
    const char *k = guest_cstr(ctx, (uint32_t)_sp[2]);
    if (L && k) lua_getfield(L, idx, k);
    else if (L) lua_pushnil(L);
    return NULL;
}

/* env.lua_setfield — v(L_h, int idx, k_off) */
static const void *m3_yos_lua_setfield(IM3Runtime rt, IM3ImportContext _c,
                                       uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    struct yos_exec_ctx *ctx = CTX(rt);
    lua_State *L = (lua_State *)lua_handles_resolve(ctx, (uint32_t)_sp[0]);
    int idx = (int)_sp[1];
    const char *k = guest_cstr(ctx, (uint32_t)_sp[2]);
    if (L && k) lua_setfield(L, idx, k);
    return NULL;
}

/* env.lua_newuserdata — i32(L_h, size_t sz).
 * Allocates Lua-managed memory of sz bytes, pushes the userdata
 * onto the stack, returns a HANDLE-style token. The userdata lives
 * in host memory the guest can't directly access; for the typical
 * "stash some bytes that the guest later identifies" pattern this is
 * sufficient because the guest passes the same userdata pointer back
 * via lua_topointer / identity-compare. Mutating the userdata
 * contents from the wasm side needs a sibling bridge we don't
 * provide yet. */
static const void *m3_yos_lua_newuserdata(IM3Runtime rt, IM3ImportContext _c,
                                          uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    lua_State *L = (lua_State *)lua_handles_resolve(CTX(rt), (uint32_t)_sp[1]);
    uint32_t sz = (uint32_t)_sp[2];
    void *p = L ? lua_newuserdata(L, sz) : NULL;
    /* Return the low 32 bits of the host pointer; comparable as an
     * opaque token only. */
    _sp[0] = (uint64_t)(uint32_t)(uintptr_t)p;
    return NULL;
}

/* ── call / pcall / load ───────────────────────────────────────── */

/* env.lua_call — v(L_h, int nargs, int nresults) */
BR_VOID_L_INT_INT(lua_call, lua_call)

/* env.lua_pcall — i32(L_h, int nargs, int nresults, int errfunc) */
static const void *m3_yos_lua_pcall(IM3Runtime rt, IM3ImportContext _c,
                                    uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    lua_State *L = (lua_State *)lua_handles_resolve(CTX(rt), (uint32_t)_sp[1]);
    int nargs    = (int)_sp[2];
    int nresults = (int)_sp[3];
    int errfunc  = (int)_sp[4];
    _sp[0] = (uint64_t)(uint32_t)(L ? lua_pcall(L, nargs, nresults, errfunc) : -1);
    return NULL;
}

/* env.luaL_loadbuffer — i32(L_h, buf_off, size_t sz, name_off) */
static const void *m3_yos_luaL_loadbuffer(IM3Runtime rt, IM3ImportContext _c,
                                          uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    struct yos_exec_ctx *ctx = CTX(rt);
    lua_State *L = (lua_State *)lua_handles_resolve(ctx, (uint32_t)_sp[1]);
    uint32_t buf_off = (uint32_t)_sp[2];
    uint32_t sz      = (uint32_t)_sp[3];
    uint32_t name_off= (uint32_t)_sp[4];
    const char *buf  = sz ? (const char *)guest_buf_ro(ctx, buf_off, sz) : "";
    const char *name = name_off ? guest_cstr(ctx, name_off) : "=(loadbuffer)";
    _sp[0] = (uint64_t)(uint32_t)((L && (buf || sz == 0)) ?
                                  luaL_loadbuffer(L, buf, sz, name) : -1);
    return NULL;
}

/* env.luaL_loadstring — i32(L_h, str_off) */
static const void *m3_yos_luaL_loadstring(IM3Runtime rt, IM3ImportContext _c,
                                          uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    struct yos_exec_ctx *ctx = CTX(rt);
    lua_State *L = (lua_State *)lua_handles_resolve(ctx, (uint32_t)_sp[1]);
    const char *s = guest_cstr(ctx, (uint32_t)_sp[2]);
    _sp[0] = (uint64_t)(uint32_t)((L && s) ? luaL_loadstring(L, s) : -1);
    return NULL;
}

/* env.luaL_loadfile — i32(L_h, filename_off).
 *
 * NOTE: this opens a file via host stdio, bypassing yos's VFS. For
 * proper sandboxing the path should route through yos_open. First
 * cut: pass the wasm-side path string straight to host. Refine when
 * a guest actually exercises a path that depends on /proc or other
 * VFS-only files. */
static const void *m3_yos_luaL_loadfile(IM3Runtime rt, IM3ImportContext _c,
                                        uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    struct yos_exec_ctx *ctx = CTX(rt);
    lua_State *L = (lua_State *)lua_handles_resolve(ctx, (uint32_t)_sp[1]);
    const char *path = guest_cstr(ctx, (uint32_t)_sp[2]);
    _sp[0] = (uint64_t)(uint32_t)((L && path) ? luaL_loadfile(L, path) : -1);
    return NULL;
}

/* env.lua_load / lua_dump — DEFERRED. Need a wasm-callback
 * trampoline for the reader/writer. Push nil + error on use. */
static const void *m3_yos_lua_load_stub(IM3Runtime rt, IM3ImportContext _c,
                                        uint64_t *_sp, void *_m)
{
    (void)rt; (void)_c; (void)_m;
    ywarn("lua_load: reader-callback bridge not implemented; "
          "use luaL_loadbuffer / luaL_loadstring / luaL_loadfile\n");
    _sp[0] = (uint64_t)(uint32_t)-1;
    return NULL;
}

/* ── GC ─────────────────────────────────────────────────────────── */
BR_INT_L_INT_INT(lua_gc, lua_gc)

/* ── coroutines ─────────────────────────────────────────────────── */
BR_INT_L_INT  (lua_resume, lua_resume)
BR_INT_L_INT  (lua_yield,  lua_yield)
/* lua_status takes 1 arg (L) */
static const void *m3_yos_lua_status(IM3Runtime rt, IM3ImportContext _c,
                                     uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    lua_State *L = (lua_State *)lua_handles_resolve(CTX(rt), (uint32_t)_sp[1]);
    _sp[0] = (uint64_t)(uint32_t)(L ? lua_status(L) : -1);
    return NULL;
}

/* ── luaL helpers ──────────────────────────────────────────────── */

BR_INT_L_INT  (luaL_ref,           luaL_ref)
BR_VOID_L_INT_INT(luaL_unref,      luaL_unref)
BR_VOID_L_INT_INT(luaL_checktype,  luaL_checktype)
BR_VOID_L_INT (luaL_checkany,      luaL_checkany)
BR_VOID_L_INT (luaL_where,         luaL_where)

/* env.luaL_newmetatable — i32(L_h, name_off) */
static const void *m3_yos_luaL_newmetatable(IM3Runtime rt, IM3ImportContext _c,
                                            uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    struct yos_exec_ctx *ctx = CTX(rt);
    lua_State *L = (lua_State *)lua_handles_resolve(ctx, (uint32_t)_sp[1]);
    const char *name = guest_cstr(ctx, (uint32_t)_sp[2]);
    _sp[0] = (uint64_t)(uint32_t)((L && name) ? luaL_newmetatable(L, name) : 0);
    return NULL;
}

/* env.luaL_getmetafield — i32(L_h, int obj, name_off) */
static const void *m3_yos_luaL_getmetafield(IM3Runtime rt, IM3ImportContext _c,
                                            uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    struct yos_exec_ctx *ctx = CTX(rt);
    lua_State *L = (lua_State *)lua_handles_resolve(ctx, (uint32_t)_sp[1]);
    int obj = (int)_sp[2];
    const char *name = guest_cstr(ctx, (uint32_t)_sp[3]);
    _sp[0] = (uint64_t)(uint32_t)((L && name) ? luaL_getmetafield(L, obj, name) : 0);
    return NULL;
}

/* env.luaL_callmeta — i32(L_h, int obj, name_off) */
static const void *m3_yos_luaL_callmeta(IM3Runtime rt, IM3ImportContext _c,
                                        uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    struct yos_exec_ctx *ctx = CTX(rt);
    lua_State *L = (lua_State *)lua_handles_resolve(ctx, (uint32_t)_sp[1]);
    int obj = (int)_sp[2];
    const char *name = guest_cstr(ctx, (uint32_t)_sp[3]);
    _sp[0] = (uint64_t)(uint32_t)((L && name) ? luaL_callmeta(L, obj, name) : 0);
    return NULL;
}

/* env.luaL_checknumber — f64(L_h, int narg) */
static const void *m3_yos_luaL_checknumber(IM3Runtime rt, IM3ImportContext _c,
                                           uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    lua_State *L = (lua_State *)lua_handles_resolve(CTX(rt), (uint32_t)_sp[1]);
    int narg = (int)_sp[2];
    double v = L ? (double)luaL_checknumber(L, narg) : 0.0;
    memcpy(&_sp[0], &v, sizeof(double));
    return NULL;
}

/* env.luaL_checkinteger — i32(L_h, int narg) */
static const void *m3_yos_luaL_checkinteger(IM3Runtime rt, IM3ImportContext _c,
                                            uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    lua_State *L = (lua_State *)lua_handles_resolve(CTX(rt), (uint32_t)_sp[1]);
    int narg = (int)_sp[2];
    _sp[0] = (uint64_t)(uint32_t)(L ? (int32_t)luaL_checkinteger(L, narg) : 0);
    return NULL;
}

/* env.luaL_checklstring — i32(L_h, int narg, len_off) */
static const void *m3_yos_luaL_checklstring(IM3Runtime rt, IM3ImportContext _c,
                                            uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    struct yos_exec_ctx *ctx = CTX(rt);
    lua_State *L = (lua_State *)lua_handles_resolve(ctx, (uint32_t)_sp[1]);
    int narg = (int)_sp[2];
    uint32_t len_off = (uint32_t)_sp[3];
    if (!L) { _sp[0] = 0; return NULL; }
    size_t len = 0;
    const char *s = luaL_checklstring(L, narg, &len);
    if (len_off) {
        uint32_t *lp = (uint32_t *)guest_buf_rw(ctx, len_off, sizeof(uint32_t));
        if (lp) *lp = (uint32_t)len;
    }
    _sp[0] = (uint64_t)guest_stash_string(ctx, s, len);
    return NULL;
}

/* env.luaL_optnumber — f64(L_h, int narg, f64 d) */
static const void *m3_yos_luaL_optnumber(IM3Runtime rt, IM3ImportContext _c,
                                         uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    lua_State *L = (lua_State *)lua_handles_resolve(CTX(rt), (uint32_t)_sp[1]);
    int narg = (int)_sp[2];
    double d; memcpy(&d, &_sp[3], sizeof(double));
    double v = L ? (double)luaL_optnumber(L, narg, (lua_Number)d) : d;
    memcpy(&_sp[0], &v, sizeof(double));
    return NULL;
}

/* env.luaL_optinteger — i32(L_h, int narg, int d) */
static const void *m3_yos_luaL_optinteger(IM3Runtime rt, IM3ImportContext _c,
                                          uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    lua_State *L = (lua_State *)lua_handles_resolve(CTX(rt), (uint32_t)_sp[1]);
    int narg = (int)_sp[2];
    int      d= (int)_sp[3];
    _sp[0] = (uint64_t)(uint32_t)(L ? (int32_t)luaL_optinteger(L, narg, d) : d);
    return NULL;
}

/* env.luaL_optlstring — i32(L_h, int narg, default_off, len_off) */
static const void *m3_yos_luaL_optlstring(IM3Runtime rt, IM3ImportContext _c,
                                          uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    struct yos_exec_ctx *ctx = CTX(rt);
    lua_State *L = (lua_State *)lua_handles_resolve(ctx, (uint32_t)_sp[1]);
    int narg = (int)_sp[2];
    const char *def = (uint32_t)_sp[3] ? guest_cstr(ctx, (uint32_t)_sp[3]) : NULL;
    uint32_t len_off = (uint32_t)_sp[4];
    if (!L) { _sp[0] = 0; return NULL; }
    size_t len = 0;
    const char *s = luaL_optlstring(L, narg, def, &len);
    if (len_off) {
        uint32_t *lp = (uint32_t *)guest_buf_rw(ctx, len_off, sizeof(uint32_t));
        if (lp) *lp = (uint32_t)len;
    }
    _sp[0] = s ? (uint64_t)guest_stash_string(ctx, s, len) : 0;
    return NULL;
}

/* env.luaL_error — i32(L_h, fmt_off, ...).
 *
 * Variadic. We can't easily marshal the variadic part across the
 * wasm/host boundary. Treat fmt as a plain string and emit it via
 * lua_pushstring + lua_error, which is what most callers actually
 * want when they pass already-formatted text. */
static const void *m3_yos_luaL_error(IM3Runtime rt, IM3ImportContext _c,
                                     uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    struct yos_exec_ctx *ctx = CTX(rt);
    lua_State *L = (lua_State *)lua_handles_resolve(ctx, (uint32_t)_sp[1]);
    const char *fmt = guest_cstr(ctx, (uint32_t)_sp[2]);
    if (!L) { _sp[0] = (uint64_t)(uint32_t)-1; return NULL; }
    lua_pushstring(L, fmt ? fmt : "luaL_error: <bad fmt offset>");
    _sp[0] = (uint64_t)(uint32_t)lua_error(L);
    return NULL;
}

/* env.luaL_argerror — i32(L_h, int numarg, msg_off) */
static const void *m3_yos_luaL_argerror(IM3Runtime rt, IM3ImportContext _c,
                                        uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    struct yos_exec_ctx *ctx = CTX(rt);
    lua_State *L = (lua_State *)lua_handles_resolve(ctx, (uint32_t)_sp[1]);
    int numarg = (int)_sp[2];
    const char *msg = guest_cstr(ctx, (uint32_t)_sp[3]);
    _sp[0] = (uint64_t)(uint32_t)((L && msg) ? luaL_argerror(L, numarg, msg) : -1);
    return NULL;
}

/* env.luaL_typerror — i32(L_h, int narg, tname_off) */
static const void *m3_yos_luaL_typerror(IM3Runtime rt, IM3ImportContext _c,
                                        uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    struct yos_exec_ctx *ctx = CTX(rt);
    lua_State *L = (lua_State *)lua_handles_resolve(ctx, (uint32_t)_sp[1]);
    int narg = (int)_sp[2];
    const char *tname = guest_cstr(ctx, (uint32_t)_sp[3]);
    _sp[0] = (uint64_t)(uint32_t)((L && tname) ? luaL_typerror(L, narg, tname) : -1);
    return NULL;
}

/* env.luaL_checkudata — i32(L_h, int ud, tname_off) */
static const void *m3_yos_luaL_checkudata(IM3Runtime rt, IM3ImportContext _c,
                                          uint64_t *_sp, void *_m)
{
    (void)_c; (void)_m;
    struct yos_exec_ctx *ctx = CTX(rt);
    lua_State *L = (lua_State *)lua_handles_resolve(ctx, (uint32_t)_sp[1]);
    int ud = (int)_sp[2];
    const char *tname = guest_cstr(ctx, (uint32_t)_sp[3]);
    void *p = (L && tname) ? luaL_checkudata(L, ud, tname) : NULL;
    _sp[0] = (uint64_t)(uint32_t)(uintptr_t)p;
    return NULL;
}

/* ── link ───────────────────────────────────────────────────────── */

void yos_liblua_link(IM3Module mod)
{
    if (!mod) return;
    /* State */
    m3_LinkRawFunction(mod, "env", "luaL_newstate",  "i()",    m3_yos_luaL_newstate);
    m3_LinkRawFunction(mod, "env", "lua_close",      "v(i)",   m3_yos_lua_close);
    m3_LinkRawFunction(mod, "env", "luaL_openlibs",  "v(i)",   m3_yos_luaL_openlibs);
    m3_LinkRawFunction(mod, "env", "lua_newthread",  "i(i)",   m3_yos_lua_newthread);
    m3_LinkRawFunction(mod, "env", "lua_status",     "i(i)",   m3_yos_lua_status);
    m3_LinkRawFunction(mod, "env", "lua_resume",     "i(ii)",  m3_yos_lua_resume);
    m3_LinkRawFunction(mod, "env", "lua_yield",      "i(ii)",  m3_yos_lua_yield);

    /* Stack */
    m3_LinkRawFunction(mod, "env", "lua_gettop",     "i(i)",   m3_yos_lua_gettop);
    m3_LinkRawFunction(mod, "env", "lua_settop",     "v(ii)",  m3_yos_lua_settop);
    m3_LinkRawFunction(mod, "env", "lua_pushvalue",  "v(ii)",  m3_yos_lua_pushvalue);
    m3_LinkRawFunction(mod, "env", "lua_remove",     "v(ii)",  m3_yos_lua_remove);
    m3_LinkRawFunction(mod, "env", "lua_insert",     "v(ii)",  m3_yos_lua_insert);
    m3_LinkRawFunction(mod, "env", "lua_replace",    "v(ii)",  m3_yos_lua_replace);
    m3_LinkRawFunction(mod, "env", "lua_checkstack", "i(ii)",  m3_yos_lua_checkstack);
    m3_LinkRawFunction(mod, "env", "lua_xmove",      "v(iii)", m3_yos_lua_xmove);

    /* Type queries */
    m3_LinkRawFunction(mod, "env", "lua_type",        "i(ii)",  m3_yos_lua_type);
    m3_LinkRawFunction(mod, "env", "lua_typename",    "i(ii)",  m3_yos_lua_typename);
    m3_LinkRawFunction(mod, "env", "lua_isnumber",    "i(ii)",  m3_yos_lua_isnumber);
    m3_LinkRawFunction(mod, "env", "lua_isstring",    "i(ii)",  m3_yos_lua_isstring);
    m3_LinkRawFunction(mod, "env", "lua_iscfunction", "i(ii)",  m3_yos_lua_iscfunction);
    m3_LinkRawFunction(mod, "env", "lua_isuserdata",  "i(ii)",  m3_yos_lua_isuserdata);
    m3_LinkRawFunction(mod, "env", "lua_rawequal",    "i(iii)", m3_yos_lua_rawequal);
    m3_LinkRawFunction(mod, "env", "lua_equal",       "i(iii)", m3_yos_lua_equal);
    m3_LinkRawFunction(mod, "env", "lua_lessthan",    "i(iii)", m3_yos_lua_lessthan);

    /* Get */
    m3_LinkRawFunction(mod, "env", "lua_tonumber",    "F(ii)",  m3_yos_lua_tonumber);
    m3_LinkRawFunction(mod, "env", "lua_tointeger",   "i(ii)",  m3_yos_lua_tointeger);
    m3_LinkRawFunction(mod, "env", "lua_toboolean",   "i(ii)",  m3_yos_lua_toboolean);
    m3_LinkRawFunction(mod, "env", "lua_tolstring",   "i(iii)", m3_yos_lua_tolstring);
    m3_LinkRawFunction(mod, "env", "lua_objlen",      "i(ii)",  m3_yos_lua_objlen);
    m3_LinkRawFunction(mod, "env", "lua_tocfunction", "i(ii)",  m3_yos_lua_tocfunction);
    m3_LinkRawFunction(mod, "env", "lua_touserdata",  "i(ii)",  m3_yos_lua_touserdata);
    m3_LinkRawFunction(mod, "env", "lua_tothread",    "i(ii)",  m3_yos_lua_tothread);
    m3_LinkRawFunction(mod, "env", "lua_topointer",   "i(ii)",  m3_yos_lua_topointer);

    /* Push */
    m3_LinkRawFunction(mod, "env", "lua_pushnil",           "v(i)",   m3_yos_lua_pushnil);
    m3_LinkRawFunction(mod, "env", "lua_pushnumber",        "v(iF)",  m3_yos_lua_pushnumber);
    m3_LinkRawFunction(mod, "env", "lua_pushinteger",       "v(ii)",  m3_yos_lua_pushinteger);
    m3_LinkRawFunction(mod, "env", "lua_pushlstring",       "v(iii)", m3_yos_lua_pushlstring);
    m3_LinkRawFunction(mod, "env", "lua_pushstring",        "v(ii)",  m3_yos_lua_pushstring);
    m3_LinkRawFunction(mod, "env", "lua_pushboolean",       "v(ii)",  m3_yos_lua_pushboolean);
    m3_LinkRawFunction(mod, "env", "lua_pushlightuserdata", "v(ii)",  m3_yos_lua_pushlightuserdata);
    m3_LinkRawFunction(mod, "env", "lua_pushthread",        "i(i)",   m3_yos_lua_pushthread);
    m3_LinkRawFunction(mod, "env", "lua_pushcclosure",      "v(iii)", m3_yos_lua_pushcclosure);

    /* Tables */
    m3_LinkRawFunction(mod, "env", "lua_createtable",  "v(iii)", m3_yos_lua_createtable);
    m3_LinkRawFunction(mod, "env", "lua_gettable",     "v(ii)",  m3_yos_lua_gettable);
    m3_LinkRawFunction(mod, "env", "lua_getfield",     "v(iii)", m3_yos_lua_getfield);
    m3_LinkRawFunction(mod, "env", "lua_rawget",       "v(ii)",  m3_yos_lua_rawget);
    m3_LinkRawFunction(mod, "env", "lua_rawgeti",      "v(iii)", m3_yos_lua_rawgeti);
    m3_LinkRawFunction(mod, "env", "lua_newuserdata",  "i(ii)",  m3_yos_lua_newuserdata);
    m3_LinkRawFunction(mod, "env", "lua_getmetatable", "i(ii)",  m3_yos_lua_getmetatable);
    m3_LinkRawFunction(mod, "env", "lua_getfenv",      "v(ii)",  m3_yos_lua_getfenv);
    m3_LinkRawFunction(mod, "env", "lua_settable",     "v(ii)",  m3_yos_lua_settable);
    m3_LinkRawFunction(mod, "env", "lua_setfield",     "v(iii)", m3_yos_lua_setfield);
    m3_LinkRawFunction(mod, "env", "lua_rawset",       "v(ii)",  m3_yos_lua_rawset);
    m3_LinkRawFunction(mod, "env", "lua_rawseti",      "v(iii)", m3_yos_lua_rawseti);
    m3_LinkRawFunction(mod, "env", "lua_setmetatable", "i(ii)",  m3_yos_lua_setmetatable);
    m3_LinkRawFunction(mod, "env", "lua_setfenv",      "i(ii)",  m3_yos_lua_setfenv);
    m3_LinkRawFunction(mod, "env", "lua_next",         "i(ii)",  m3_yos_lua_next);
    m3_LinkRawFunction(mod, "env", "lua_concat",       "v(ii)",  m3_yos_lua_concat);

    /* Call / load / error */
    m3_LinkRawFunction(mod, "env", "lua_call",         "v(iii)",   m3_yos_lua_call);
    m3_LinkRawFunction(mod, "env", "lua_pcall",        "i(iiii)",  m3_yos_lua_pcall);
    m3_LinkRawFunction(mod, "env", "lua_error",        "i(i)",     m3_yos_lua_error);
    m3_LinkRawFunction(mod, "env", "lua_load",         "i(iiii)",  m3_yos_lua_load_stub);
    m3_LinkRawFunction(mod, "env", "lua_dump",         "i(iii)",   m3_yos_lua_load_stub);
    m3_LinkRawFunction(mod, "env", "luaL_loadbuffer",  "i(iiii)",  m3_yos_luaL_loadbuffer);
    m3_LinkRawFunction(mod, "env", "luaL_loadstring",  "i(ii)",    m3_yos_luaL_loadstring);
    m3_LinkRawFunction(mod, "env", "luaL_loadfile",    "i(ii)",    m3_yos_luaL_loadfile);

    /* GC */
    m3_LinkRawFunction(mod, "env", "lua_gc", "i(iii)", m3_yos_lua_gc);

    /* luaL */
    m3_LinkRawFunction(mod, "env", "luaL_ref",            "i(ii)",   m3_yos_luaL_ref);
    m3_LinkRawFunction(mod, "env", "luaL_unref",          "v(iii)",  m3_yos_luaL_unref);
    m3_LinkRawFunction(mod, "env", "luaL_checktype",      "v(iii)",  m3_yos_luaL_checktype);
    m3_LinkRawFunction(mod, "env", "luaL_checkany",       "v(ii)",   m3_yos_luaL_checkany);
    m3_LinkRawFunction(mod, "env", "luaL_where",          "v(ii)",   m3_yos_luaL_where);
    m3_LinkRawFunction(mod, "env", "luaL_newmetatable",   "i(ii)",   m3_yos_luaL_newmetatable);
    m3_LinkRawFunction(mod, "env", "luaL_checkudata",     "i(iii)",  m3_yos_luaL_checkudata);
    m3_LinkRawFunction(mod, "env", "luaL_getmetafield",   "i(iii)",  m3_yos_luaL_getmetafield);
    m3_LinkRawFunction(mod, "env", "luaL_callmeta",       "i(iii)",  m3_yos_luaL_callmeta);
    m3_LinkRawFunction(mod, "env", "luaL_checknumber",    "F(ii)",   m3_yos_luaL_checknumber);
    m3_LinkRawFunction(mod, "env", "luaL_checkinteger",   "i(ii)",   m3_yos_luaL_checkinteger);
    m3_LinkRawFunction(mod, "env", "luaL_checklstring",   "i(iii)",  m3_yos_luaL_checklstring);
    m3_LinkRawFunction(mod, "env", "luaL_optnumber",      "F(iiF)",  m3_yos_luaL_optnumber);
    m3_LinkRawFunction(mod, "env", "luaL_optinteger",     "i(iii)",  m3_yos_luaL_optinteger);
    m3_LinkRawFunction(mod, "env", "luaL_optlstring",     "i(iiii)", m3_yos_luaL_optlstring);
    m3_LinkRawFunction(mod, "env", "luaL_error",          "i(ii)",   m3_yos_luaL_error);
    m3_LinkRawFunction(mod, "env", "luaL_argerror",       "i(iii)",  m3_yos_luaL_argerror);
    m3_LinkRawFunction(mod, "env", "luaL_typerror",       "i(iii)",  m3_yos_luaL_typerror);
}

/* Per-ctx teardown — close any lua_State still in the handle table.
 * Called from yos's proc shutdown so a guest that exits without
 * lua_close doesn't leak the host state. */
void yos_liblua_ctx_free(struct yos_exec_ctx *ctx)
{
    if (!ctx || !ctx->lua_handles) return;
    /* Close only the MAIN state (typically handle 1). Closing the
     * main state cascades and frees all coroutines automatically;
     * calling lua_close on a coroutine is undefined behaviour. We
     * conservatively close handle 1 only. */
    if (ctx->lua_handles_cap > 1 && ctx->lua_handles[1]) {
        lua_close((lua_State *)ctx->lua_handles[1]);
    }
    free(ctx->lua_handles);
    ctx->lua_handles = NULL;
    ctx->lua_handles_cap = 0;
}
