/* impl/printf.c — host-side printf family.
 *
 * clang's wasm32 ABI lowers a variadic call `printf(fmt, x, y, …)` into
 * a non-variadic call `printf(fmt_ptr, va_list_ptr)`. The caller packs
 * the varargs into the wasm shadow stack at `va_list_ptr`, with each
 * argument 8-byte-aligned (so an i32 occupies 8 bytes; an f64 occupies
 * 8 bytes; an i64 occupies 8 bytes).
 *
 * We can't build a host `va_list` portably from raw memory. Instead
 * we walk the format string ourselves: for each conversion spec, read
 * the matching slot(s) from `ctx->memory + va_list_ptr`, call host
 * `snprintf("%<spec>", val)` to format that single piece, and copy
 * the result into an output buffer. Strings (`%s`) we read from the
 * guest's linear memory.
 *
 * Limitations of this first cut:
 *   - `%n` is rejected (security; rarely used).
 *   - Wide-string formatters (`%ls`, `%C`) — passthrough as `%s` /
 *     `%c`. nvim doesn't use them.
 *   - Field width / precision passed via `*` arg are honoured via
 *     reading the extra slot.
 *   - `%a`/`%A` are passed through as host snprintf.
 *
 * Output:
 *   yos_vsnprintf(ctx, dst, n, fmt, ap) — formats into wasm-side buf
 *   yos_vfprintf (ctx, fp, fmt, ap)     — formats and fwrites to host FILE*
 *   yos_printf, yos_fprintf, yos_sprintf, yos_snprintf, yos_vprintf,
 *   yos_vsprintf — thin wrappers around the above, fed by hand-written
 *   m3w_<name> trampolines bound in main.c. */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "yos/types.h"

/* Read one slot from the guest's va_list region.
 *
 * Clang's wasm32 variadic ABI packs each variadic arg at its NATURAL
 * alignment, then advances by the type's size — NOT a fixed 8-byte
 * slot. So:
 *   int / pointer (i32):   align 4, size 4
 *   long long (i64):       align 8, size 8
 *   double (f64):           align 8, size 8
 *
 * We were treating every slot as 8 bytes, which silently drifted the
 * read offset for any format like `"%s_%d"` (the `%d` ended up reading
 * past the int into adjacent memory). The clearenv FreeBSD test pinned
 * this — `snprintf("%s_%d", "TEST", i)` always returned "TEST_0". */
static inline void va_align(uint32_t *off, uint32_t a) {
    *off = (*off + a - 1) & ~(a - 1);
}
static inline uint32_t va_i32(struct yos_exec_ctx *ctx, uint32_t *off) {
    va_align(off, 4);
    uint32_t v = *(uint32_t *)(ctx->memory + *off);
    *off += 4;
    return v;
}
static inline uint64_t va_i64(struct yos_exec_ctx *ctx, uint32_t *off) {
    va_align(off, 8);
    uint64_t v = *(uint64_t *)(ctx->memory + *off);
    *off += 8;
    return v;
}
static inline double va_f64(struct yos_exec_ctx *ctx, uint32_t *off) {
    va_align(off, 8);
    double v = *(double *)(ctx->memory + *off);
    *off += 8;
    return v;
}

/* Parse a single conversion spec starting at *fmt, append the formatted
 * result to `out` (capped at `out_cap` bytes total in `*outpos`). On
 * return *fmt points at the byte AFTER the spec. */
static void format_one(struct yos_exec_ctx *ctx,
                       const char **fmt, uint32_t *vap,
                       char *out, size_t out_cap, size_t *outpos)
{
    /* Copy the full spec verbatim into a scratch buffer so we can
     * pass it straight to host snprintf. Specs include flags, width,
     * precision, length modifier, conversion char. */
    char spec[64];
    size_t sp = 0;
    spec[sp++] = '%';
    const char *p = *fmt + 1;  /* past the '%' */
    /* flags */
    while (sp + 1 < sizeof(spec) && strchr("-+ #0'", *p)) spec[sp++] = *p++;
    /* width — may be '*' */
    int width_star = 0, prec_star = 0;
    if (*p == '*') {
        spec[sp++] = '*'; p++;
        width_star = 1;
    } else {
        while (sp + 1 < sizeof(spec) && *p >= '0' && *p <= '9')
            spec[sp++] = *p++;
    }
    /* precision */
    if (*p == '.') {
        spec[sp++] = *p++;
        if (*p == '*') {
            spec[sp++] = *p++;
            prec_star = 1;
        } else {
            while (sp + 1 < sizeof(spec) && *p >= '0' && *p <= '9')
                spec[sp++] = *p++;
        }
    }
    /* length modifier */
    int is_ll = 0, is_l = 0, is_h = 0, is_z = 0, is_j = 0;
    while (sp + 1 < sizeof(spec) && strchr("hlLjztq", *p)) {
        if (*p == 'l' && p[1] == 'l') { is_ll = 1; spec[sp++] = *p++; }
        else if (*p == 'l') { is_l = 1; }
        else if (*p == 'h') { is_h = 1; }
        else if (*p == 'z') { is_z = 1; }
        else if (*p == 'j') { is_j = 1; }
        spec[sp++] = *p++;
    }
    /* conversion */
    char conv = *p;
    if (sp + 2 >= sizeof(spec) || conv == '\0') { *fmt = p; return; }
    spec[sp++] = conv;
    spec[sp] = '\0';
    *fmt = p + 1;

    /* Pull star args first (host snprintf needs them in order). */
    int star_w = 0, star_p = 0;
    if (width_star) star_w = (int)va_i32(ctx, vap);
    if (prec_star)  star_p = (int)va_i32(ctx, vap);

    char buf[1024];
    int n = 0;

    switch (conv) {
    case 'd': case 'i': {
        if (is_ll || is_j) {
            int64_t v = (int64_t)va_i64(ctx, vap);
            n = width_star && prec_star ? snprintf(buf, sizeof(buf), spec, star_w, star_p, (long long)v)
              : width_star              ? snprintf(buf, sizeof(buf), spec, star_w, (long long)v)
              : prec_star               ? snprintf(buf, sizeof(buf), spec, star_p, (long long)v)
              :                            snprintf(buf, sizeof(buf), spec, (long long)v);
        } else {
            int32_t v = (int32_t)va_i32(ctx, vap);
            n = width_star && prec_star ? snprintf(buf, sizeof(buf), spec, star_w, star_p, (int)v)
              : width_star              ? snprintf(buf, sizeof(buf), spec, star_w, (int)v)
              : prec_star               ? snprintf(buf, sizeof(buf), spec, star_p, (int)v)
              :                            snprintf(buf, sizeof(buf), spec, (int)v);
        }
        break;
    }
    case 'u': case 'x': case 'X': case 'o': {
        if (is_ll || is_j) {
            uint64_t v = va_i64(ctx, vap);
            n = width_star && prec_star ? snprintf(buf, sizeof(buf), spec, star_w, star_p, (unsigned long long)v)
              : width_star              ? snprintf(buf, sizeof(buf), spec, star_w, (unsigned long long)v)
              : prec_star               ? snprintf(buf, sizeof(buf), spec, star_p, (unsigned long long)v)
              :                            snprintf(buf, sizeof(buf), spec, (unsigned long long)v);
        } else {
            uint32_t v = va_i32(ctx, vap);
            n = width_star && prec_star ? snprintf(buf, sizeof(buf), spec, star_w, star_p, (unsigned)v)
              : width_star              ? snprintf(buf, sizeof(buf), spec, star_w, (unsigned)v)
              : prec_star               ? snprintf(buf, sizeof(buf), spec, star_p, (unsigned)v)
              :                            snprintf(buf, sizeof(buf), spec, (unsigned)v);
        }
        break;
    }
    case 'p': {
        uint32_t v = va_i32(ctx, vap);
        /* Render guest pointers as their wasm offset in hex, like
         * host %p — keeps debug output readable. */
        n = snprintf(buf, sizeof(buf), "0x%x", v);
        break;
    }
    case 'c': {
        uint32_t v = va_i32(ctx, vap);
        n = snprintf(buf, sizeof(buf), spec, (int)v);
        break;
    }
    case 's': {
        uint32_t s_off = va_i32(ctx, vap);
        const char *s = s_off ? (const char *)(ctx->memory + s_off) : "(null)";
        n = width_star && prec_star ? snprintf(buf, sizeof(buf), spec, star_w, star_p, s)
          : width_star              ? snprintf(buf, sizeof(buf), spec, star_w, s)
          : prec_star               ? snprintf(buf, sizeof(buf), spec, star_p, s)
          :                            snprintf(buf, sizeof(buf), spec, s);
        break;
    }
    case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
    case 'a': case 'A': {
        double v = va_f64(ctx, vap);
        n = width_star && prec_star ? snprintf(buf, sizeof(buf), spec, star_w, star_p, v)
          : width_star              ? snprintf(buf, sizeof(buf), spec, star_w, v)
          : prec_star               ? snprintf(buf, sizeof(buf), spec, star_p, v)
          :                            snprintf(buf, sizeof(buf), spec, v);
        break;
    }
    case '%': {
        buf[0] = '%'; buf[1] = '\0'; n = 1; break;
    }
    case 'n':
        /* Refuse — would let guest write through a pointer arg. */
        n = 0; break;
    default:
        /* Unknown conversion — emit literally. */
        n = snprintf(buf, sizeof(buf), "%%%c", conv);
    }
    if (n < 0) return;
    size_t cp = (size_t)n;
    if (*outpos + cp > out_cap) cp = out_cap > *outpos ? out_cap - *outpos : 0;
    memcpy(out + *outpos, buf, cp);
    *outpos += cp;
}

/* Format `fmt` (with vararg slots starting at wasm offset `vap`) into
 * a host buffer. Returns the number of bytes that would have been
 * written if `out_cap` were unbounded (snprintf semantics). */
int yos_vsnprintf_core(struct yos_exec_ctx *ctx,
                       char *out, size_t out_cap,
                       uint32_t fmt_off, uint32_t va_off)
{
    const char *fmt = (const char *)(ctx->memory + fmt_off);
    size_t pos = 0;
    while (*fmt) {
        if (*fmt != '%') {
            if (pos < out_cap) out[pos] = *fmt;
            pos++;
            fmt++;
            continue;
        }
        format_one(ctx, &fmt, &va_off, out, out_cap, &pos);
    }
    if (out_cap > 0) out[pos < out_cap ? pos : out_cap - 1] = '\0';
    return (int)pos;
}

/* Resolve a guest FILE* handle to the host FILE* through impl/file.c's
 * handle table. Pre-bound: 1=stdin, 2=stdout, 3=stderr; 4..MAX are
 * fopen-allocated. Anything unknown falls back to stdout — partial
 * output is less bad than a NULL deref in fwrite. nvim's logger
 * writes to a real fopen()'d file (handle ≥ 4), and a previous shim
 * here was hard-coding 1→stdout / 2→stderr / else→stdout, sending
 * every log line to stdout instead. */
extern FILE *yos_handle_to_file(uint32_t h);
static FILE *guest_fp_to_host(struct yos_exec_ctx *ctx, uint32_t fp_off)
{
    (void)ctx;
    if (fp_off == 0) return stdout;
    FILE *f = yos_handle_to_file(fp_off);
    return f ? f : stdout;
}

int32_t yos_vfprintf(struct yos_exec_ctx *ctx,
                     uint32_t fp, uint32_t fmt_off, uint32_t va_off)
{
    char buf[8192];
    int n = yos_vsnprintf_core(ctx, buf, sizeof(buf), fmt_off, va_off);
    if (n < 0) return -1;
    size_t w = n < (int)sizeof(buf) ? (size_t)n : sizeof(buf) - 1;
    FILE *f = guest_fp_to_host(ctx, fp);
    fwrite(buf, 1, w, f);
    return n;
}

int32_t yos_fprintf(struct yos_exec_ctx *ctx,
                    uint32_t fp, uint32_t fmt_off, uint32_t va_off)
{
    return yos_vfprintf(ctx, fp, fmt_off, va_off);
}

/* impl/file.c reserves handle 2 = stdout (1 = stdin, 3 = stderr); using
 * 1 here used to silently send every printf to host stdin, which dropped
 * `nvim --version` etc. on the floor. */
int32_t yos_printf(struct yos_exec_ctx *ctx,
                   uint32_t fmt_off, uint32_t va_off)
{
    return yos_vfprintf(ctx, 2, fmt_off, va_off);
}

int32_t yos_vprintf(struct yos_exec_ctx *ctx,
                    uint32_t fmt_off, uint32_t va_off)
{
    return yos_vfprintf(ctx, 2, fmt_off, va_off);
}

int32_t yos_vsnprintf(struct yos_exec_ctx *ctx,
                      uint32_t dst_off, uint32_t n,
                      uint32_t fmt_off, uint32_t va_off)
{
    char *dst = (char *)(ctx->memory + dst_off);
    return yos_vsnprintf_core(ctx, dst, n, fmt_off, va_off);
}

int32_t yos_snprintf(struct yos_exec_ctx *ctx,
                     uint32_t dst_off, uint32_t n,
                     uint32_t fmt_off, uint32_t va_off)
{
    return yos_vsnprintf(ctx, dst_off, n, fmt_off, va_off);
}

int32_t yos_vsprintf(struct yos_exec_ctx *ctx,
                     uint32_t dst_off, uint32_t fmt_off, uint32_t va_off)
{
    /* sprintf has no bound; pretend the caller's buffer is huge. The
     * guest's libc would normally check and trap; we mirror that risk. */
    return yos_vsnprintf(ctx, dst_off, (uint32_t)-1, fmt_off, va_off);
}

int32_t yos_sprintf(struct yos_exec_ctx *ctx,
                    uint32_t dst_off, uint32_t fmt_off, uint32_t va_off)
{
    return yos_vsprintf(ctx, dst_off, fmt_off, va_off);
}
