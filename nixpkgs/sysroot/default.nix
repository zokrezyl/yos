{ stdenv, lib, python3, llvmPackages_18, src, freebsd-src }:

# Layer 2: the wasm32 sysroot the guest builds against.
#
# Mirrors what `meson compile -C build-linux sysroot-skel` does, but
# inside a Nix sandbox so the output is reproducible and content-
# addressed:
#
#   1. fetch FreeBSD's src.txz (sha256-pinned, mirrors the values in
#      meson_options.txt — kept in sync manually, see WARNING below)
#   2. extract just the parts install_includes.py reads
#   3. run build-tools/freebsd/install_includes.py to curate
#      headers-i386/usr/include/
#   4. mirror build-tools/sysroot/skel.sh:
#        - symlink usr/include from the curated tree
#        - build empty `lib<X>.a` stubs (dl, m, util, pthread, rt, c,
#          c++, cxx, anl, crypt, resolv) so wasm-ld is happy with
#          `-lX` references the recipe link lines carry
#        - compile build-tools/sysroot/skel.sh's inline crt1.c into
#          $out/usr/lib/crt1.o
#
# Output layout (matches what tools/wasm-pkg.sh sets WASM_SYSROOT to):
#   $out/
#     usr/
#       include/   ← curated FreeBSD-i386 headers
#       lib/
#         crt1.o          (yos-flavoured)
#         lib<dl,m,…>.a   (empty stubs; symbols resolve as imports)
#
# WARNING: the freebsd_version / freebsd_src_sha256 below MUST stay in
# sync with meson_options.txt. There is no automatic check — if the
# meson options bump and this file doesn't, `nix build` will fetch a
# different tarball than `meson compile` and the two paths will produce
# different sysroots.

let
  llvm = llvmPackages_18;
  clangBin = llvm.clang-unwrapped;
in stdenv.mkDerivation {
  pname = "yos-sysroot";
  version = freebsd-src.version;

  inherit src;

  nativeBuildInputs = [ python3 clangBin llvm.libllvm ];

  dontStrip = true;
  dontPatchELF = true;
  dontFixup = true;

  dontConfigure = true;

  buildPhase = ''
    runHook preBuild

    # ── 1. Use the shared freebsd-src derivation ─────────────────────
    SRC_ROOT="${freebsd-src}/usr/src"

    # ── 2. Run install_includes.py to make the curated header tree ───
    # install_includes.py writes <out_root>/usr/include/... (not
    # <out_root>/headers-i386/usr/include/... — the headers-i386/
    # prefix in the comment refers to meson's custom_target output
    # name, not the script's output layout).
    HEADERS_OUT="$TMPDIR/headers"
    python3 "$src/build-tools/freebsd/install_includes.py" \
        --src-root "$SRC_ROOT" \
        --out-root "$HEADERS_OUT" \
        --arch i386
    # FreeBSD source comes from a read-only Nix store path; shutil's
    # copy2 in install_includes.py preserves the source mode bits, so
    # the curated tree ends up read-only too. The script's own
    # post-copy patches (cdefs.h shim, etc.) then trip on
    # PermissionError. Chmod the output writable before any
    # in-place edits would fire — safe because we own the dir.
    chmod -R u+w "$HEADERS_OUT"

    # ── 3. Mirror skel.sh: sysroot skeleton + stubs + crt1 ────────────
    mkdir -p "$out/usr/lib"

    # Headers: copy (rather than symlink) so $out is self-contained
    # and the dependency graph is tight.
    cp -r "$HEADERS_OUT/usr/include" "$out/usr/include"

    # Empty .a stubs for libraries every Linux build system asks for
    # via -ldl / -lm / -lutil / -lpthread / -lrt. yos resolves all
    # libc fns at wasm load time as `env.<name>` imports — there's
    # nothing to actually link, but wasm-ld still wants the .a to
    # exist when it sees `-lX`.
    EMPTY_C="$TMPDIR/.empty.c"
    EMPTY_O="$TMPDIR/.empty.o"
    : > "$EMPTY_C"
    clang -target wasm32-unknown-unknown -nostdlib -c "$EMPTY_C" -o "$EMPTY_O"
    for libname in dl m util pthread rt c c++ cxx anl crypt resolv; do
      llvm-ar rcs "$out/usr/lib/lib''${libname}.a" "$EMPTY_O"
    done

    # crt1.o — yos-flavoured. The C source lives inline in
    # build-tools/sysroot/skel.sh; extract the heredoc and compile it
    # so the two paths stay byte-identical.
    CRT1_C="$TMPDIR/crt1.c"
    awk '/cat > "\$crt1_c" <<'"'"'EOF'"'"'/{flag=1; next} /^EOF$/{flag=0} flag' \
        "$src/build-tools/sysroot/skel.sh" > "$CRT1_C"
    if [ ! -s "$CRT1_C" ]; then
      echo "sysroot: failed to extract crt1.c from skel.sh" >&2
      exit 1
    fi
    clang -target wasm32-unknown-unknown -nostdlib -O2 \
        -c "$CRT1_C" -o "$out/usr/lib/crt1.o"

    # libyos_stubs.a — yos-side shims for FreeBSD libc/sys symbols that
    # we don't bridge but every consumer expects to link against. Right
    # now: the underlying `__cap_rights_*` family that <sys/capsicum.h>'s
    # macros call into. Tools that pull in capsicum (cat, vi, sshd, …)
    # link this archive and the macros expand to these no-ops at
    # runtime. Tools that don't reference these symbols link against
    # the archive harmlessly — linker drops unused .o's.
    YOS_STUBS_C="$TMPDIR/yos_capsicum_stubs.c"
    cat > "$YOS_STUBS_C" <<'STUBS_EOF'
    #include <stdarg.h>
    #include <stdbool.h>
    #include <stdio.h>
    #include <stdlib.h>
    #include <string.h>
    #include <errno.h>
    #include <fcntl.h>
    #include <unistd.h>
    #include <dirent.h>
    #include <sys/stat.h>
    #include <sys/mount.h>
    #include <sys/capsicum.h>

    /* ── FreeBSD libc-internal aliases ────────────────────────────────
     * FreeBSD libc has two flavours of every syscall wrapper: a public
     * one (open/close/fstat/…) plus a hidden alias prefixed with `_`
     * (e.g. `_open`). Internal libc code (fts.c, opendir2.c, …) calls
     * the underscored variant directly to bypass user interposers.
     *
     * When we compile pieces of FreeBSD libc straight into a tool
     * (libcExtras = [ "fts", "qsort", … ]), those underscored
     * references show up as undefined symbols at link time. yos's
     * bridge surface only knows the public POSIX names — wire the
     * aliases here as thin pass-throughs. The wasm linker drops the
     * .o when the tool doesn't reference these symbols, so tools that
     * don't pull in fts.c pay nothing.
     *
     * __opendir2 is FreeBSD's "real" opendir that takes a DTF_* flag
     * (whiteout / dup filtering). yos has no union-fs concept; ignore
     * the flag and route to opendir(). */
    int _open(const char *path, int flags, mode_t mode)
    { return open(path, flags, mode); }
    int _close(int fd)              { return close(fd); }
    int _fstat(int fd, struct stat *sb)            { return fstat(fd, sb); }
    int _fstatfs(int fd, struct statfs *sb)        { return fstatfs(fd, sb); }
    DIR *__opendir2(const char *name, int flag)
    { (void)flag; return opendir(name); }

    /* ── capsicum stubs ───────────────────────────────────────────── */
    cap_rights_t *
    __cap_rights_init(int version, cap_rights_t *rights, ...)
    {
        (void)version;
        if (rights) {
            rights->cr_rights[0] = 0;
            rights->cr_rights[1] = 0;
        }
        return rights;
    }
    cap_rights_t *__cap_rights_set(cap_rights_t *r, ...)   { return r; }
    cap_rights_t *__cap_rights_clear(cap_rights_t *r, ...) { return r; }
    bool __cap_rights_is_set(const cap_rights_t *r, ...)
    { (void)r; return true; }

    /* ── err(3) family — minimal impl. FreeBSD libc's err.c uses
     * the namespace.h #define err -> _err trick which doesn't link
     * cleanly when pulled into our tool builds. Provide bare
     * implementations against printf + exit so any tool that links
     * libyos_stubs gets functional err/warn/errc/warnc/verr/etc. */
    static const char *yos_progname = "yos-tool";
    void setprogname(const char *n) { yos_progname = n; }
    const char *getprogname(void)   { return yos_progname; }

    void warn(const char *fmt, ...)
    {
        int saved = errno;
        fprintf(stderr, "%s: ", yos_progname);
        if (fmt) {
            va_list ap; va_start(ap, fmt);
            vfprintf(stderr, fmt, ap);
            va_end(ap);
            fprintf(stderr, ": ");
        }
        /* yos's strerror() bridge isn't wired yet (returns NULL).
         * Fall back to printing errno numerically so the caller can
         * still tell what happened. Once strerror is bridged this
         * stays as a useful fallback for unknown errno values. */
        {
            const char *m = strerror(saved);
            if (m) fprintf(stderr, "%s\n", m);
            else   fprintf(stderr, "errno=%d\n", saved);
        }
    }
    void warnc(int code, const char *fmt, ...)
    {
        fprintf(stderr, "%s: ", yos_progname);
        if (fmt) {
            va_list ap; va_start(ap, fmt);
            vfprintf(stderr, fmt, ap);
            va_end(ap);
            fprintf(stderr, ": ");
        }
        fprintf(stderr, "%s\n", strerror(code));
    }
    void warnx(const char *fmt, ...)
    {
        fprintf(stderr, "%s: ", yos_progname);
        if (fmt) {
            va_list ap; va_start(ap, fmt);
            vfprintf(stderr, fmt, ap);
            va_end(ap);
        }
        fprintf(stderr, "\n");
    }
    void err(int eval, const char *fmt, ...)
    {
        int saved = errno;
        fprintf(stderr, "%s: ", yos_progname);
        if (fmt) {
            va_list ap; va_start(ap, fmt);
            vfprintf(stderr, fmt, ap);
            va_end(ap);
            fprintf(stderr, ": ");
        }
        /* yos's strerror() bridge returns NULL until the codegen
         * pipeline grows complex-arg/return support; fall back to
         * printing errno numerically. Useful even after strerror is
         * wired up — bare numeric errno tells you exactly what the
         * kernel returned without strerror's mapping massaging it. */
        fprintf(stderr, "errno=%d\n", saved);
        exit(eval);
    }
    void errc(int eval, int code, const char *fmt, ...)
    {
        fprintf(stderr, "%s: ", yos_progname);
        if (fmt) {
            va_list ap; va_start(ap, fmt);
            vfprintf(stderr, fmt, ap);
            va_end(ap);
            fprintf(stderr, ": ");
        }
        fprintf(stderr, "%s\n", strerror(code));
        exit(eval);
    }
    void errx(int eval, const char *fmt, ...)
    {
        fprintf(stderr, "%s: ", yos_progname);
        if (fmt) {
            va_list ap; va_start(ap, fmt);
            vfprintf(stderr, fmt, ap);
            va_end(ap);
        }
        fprintf(stderr, "\n");
        exit(eval);
    }

    /* ── termcap stubs — see <termcap.h> in this sysroot ──────────────
     * yos has no terminfo database, no termcap entries, no tty
     * capability discovery at all. We provide no-op stubs so any
     * tool that dlinked against -ltermcap (or now: links the empty
     * libtermcap.a stub from this sysroot) gets clean compile + link
     * + runtime "no terminal" behaviour. Callers fall back to plain
     * text output. */
    char  PC = 0;
    char *UP = 0;
    char *BC = 0;
    short ospeed = 0;
    int   tgetent(char *bp, const char *name)
    { (void)bp; (void)name; return -1; }   /* "no entry" */
    int   tgetnum(const char *id)          { (void)id; return -1; }
    int   tgetflag(const char *id)         { (void)id; return 0; }
    char *tgetstr(const char *id, char **area)
    { (void)id; (void)area; return 0; }
    char *tgoto(const char *cap, int col, int row)
    { (void)col; (void)row; return (char *)cap; }
    int   tputs(const char *str, int affcnt, int (*putcfn)(int))
    {
        (void)affcnt;
        if (str && putcfn) for (; *str; ++str) putcfn((unsigned char)*str);
        return 0;
    }

    /* ── stdio FILE* singletons ─────────────────────────────────────
     * FreeBSD's stdio.h declares
     *     extern FILE *__stdinp, *__stdoutp, *__stderrp;
     *     #define stdin  __stdinp
     *     #define stdout __stdoutp
     *     #define stderr __stderrp
     * The wasm guest's libc (zsh, FreeBSD coreutils, …) needs concrete
     * values for these symbols. Without them the link uses
     * --allow-undefined and the symbols resolve to 0 at runtime; every
     * fputs/fwrite/fprintf call to "stdout" passes 0 to the bridge,
     * which yos's FILE-handle table treats as "unknown handle" — output
     * silently dropped.
     *
     * yos's bridge (src/yos/impl/file.c::yos_handle_to_file) reserves
     * the small ints 1/2/3 as the canonical handles for stdin/stdout/
     * stderr — it returns the host's stdin/stdout/stderr unconditionally
     * for those values. Cast each integer to FILE* so consumers can
     * dereference-equality against the symbol but the bridge sees
     * exactly 1/2/3 as the wasm-side argument. The struct __sFILE is
     * never accessed by the host (we never read members from the wasm
     * guest's FILE), so the "pointer" is purely an opaque handle. */
    FILE *__stdinp  = (FILE *)1;
    FILE *__stdoutp = (FILE *)2;
    FILE *__stderrp = (FILE *)3;
    STUBS_EOF
    YOS_STUBS_O="$TMPDIR/yos_capsicum_stubs.o"
    # clang-unwrapped doesn't ship its resource dir on the include
    # search path; with -nostdinc, <stdarg.h> stops resolving. Add
    # -idirafter to bring back JUST the resource includes (clang
    # builtins like stdarg.h, stddef.h) without restoring the host
    # libc paths -nostdinc was meant to drop.
    CLANG_RESOURCE_DIR="${llvm.clang-unwrapped.lib}/lib/clang/18"
    clang -target wasm32-unknown-unknown -nostdlib -nostdinc -O2 \
        --sysroot="$out" -isystem "$out/usr/include" \
        -idirafter "$CLANG_RESOURCE_DIR/include" \
        -D__i386__=1 -ffreestanding \
        -c "$YOS_STUBS_C" -o "$YOS_STUBS_O"
    llvm-ar rcs "$out/usr/lib/libyos_stubs.a" "$YOS_STUBS_O"

    # ── 4. Stub headers for FreeBSD-only sandboxing facilities ───────
    # FreeBSD-base utilities (echo, cat, sh, …) call into Capsicum
    # via <capsicum_helpers.h> and <libcasper.h> for sandboxing
    # (`caph_limit_stdio`, `caph_enter`, …). yos doesn't implement
    # Capsicum — it's a kernel-level facility and our libc surface
    # exposes no equivalent — so every call must compile and behave
    # like a no-op success. Provide stub headers that satisfy the
    # API consumers expect; the symbols expand to inline no-ops.
    cat > "$out/usr/include/capsicum_helpers.h" <<'CAPH_EOF'
    /* yos stub — Capsicum sandboxing isn't implemented.
     * Every helper compiles to a no-op success so FreeBSD-base
     * utilities that sandbox themselves keep building and running. */
    #ifndef _CAPSICUM_HELPERS_H_
    #define _CAPSICUM_HELPERS_H_
    #include <stddef.h>
    static inline int caph_limit_stdin(void)   { return 0; }
    static inline int caph_limit_stdout(void)  { return 0; }
    static inline int caph_limit_stderr(void)  { return 0; }
    static inline int caph_limit_stdio(void)   { return 0; }
    static inline int caph_enter(void)         { return 0; }
    static inline int caph_enter_casper(void)  { return 0; }
    static inline int caph_rights_limit(int fd, const void *r) { (void)fd; (void)r; return 0; }
    static inline int caph_ioctls_limit(int fd, const unsigned long *cmds, size_t n)
        { (void)fd; (void)cmds; (void)n; return 0; }
    static inline int caph_fcntls_limit(int fd, unsigned long fc) { (void)fd; (void)fc; return 0; }
    /* caph_cache_*(): one-shot warm-up calls some tools issue before
     * sandbox entry to make sure libc has loaded its lazy data tables
     * (catpages = nl_catopen tables, tzdata, dns config). No-op here:
     * we don't sandbox, so there's no need to pre-warm. */
    static inline void caph_cache_catpages(void)         { }
    static inline void caph_cache_tzdata(void)           { }
    static inline void caph_cache_dns_conf(void)         { }
    #endif
    CAPH_EOF
    cat > "$out/usr/include/libcasper.h" <<'CAS_EOF'
    /* yos stub — libcasper isn't implemented. Tools that take the
     * Capsicum/Casper path generally do `cap_init()` and bail if
     * NULL; we return a non-NULL sentinel so they proceed. The
     * actual operations (cap_service_open, cap_net_*, fileargs_*)
     * are stubbed in their respective service headers and pass
     * through to the underlying libc calls. Sandboxing isn't
     * enforced, but the code paths run. */
    #ifndef _LIBCASPER_H_
    #define _LIBCASPER_H_
    #include <stddef.h>
    typedef struct cap_channel cap_channel_t;
    static inline cap_channel_t *cap_init(void)             { return (cap_channel_t *)0x1; }
    static inline void           cap_close(cap_channel_t *c){ (void)c; }
    static inline cap_channel_t *cap_service_open(cap_channel_t *c, const char *s)
        { (void)c; (void)s; return (cap_channel_t *)0x1; }
    #endif
    CAS_EOF
    mkdir -p "$out/usr/include/casper"
    cat > "$out/usr/include/casper/cap_fileargs.h" <<'CFA_EOF'
    /* yos stub — Casper file-args service. Real implementation
     * sandboxes a process and delegates open(2) / realpath(3) to a
     * privileged Casper helper that holds rights on a vetted set of
     * paths. We don't sandbox; the helpers fall through to the
     * underlying libc calls so file access just works as if the
     * caller never had Capsicum. */
    #ifndef _CAP_FILEARGS_H_
    #define _CAP_FILEARGS_H_
    #include <fcntl.h>
    #include <stdlib.h>
    #include <unistd.h>
    typedef struct fileargs       fileargs_t;
    typedef struct cap_rights     cap_rights_t;
    static inline fileargs_t *
    fileargs_cinit(cap_channel_t *casper, int argc, char **argv, int flags, mode_t mode,
                   const cap_rights_t *rights, int operations)
    {
        (void)casper; (void)argc; (void)argv;
        (void)flags; (void)mode; (void)rights; (void)operations;
        return (fileargs_t *)0x1;
    }
    static inline fileargs_t *
    fileargs_init(int argc, char **argv, int flags, mode_t mode,
                  const cap_rights_t *rights, int operations)
    {
        (void)argc; (void)argv; (void)flags;
        (void)mode; (void)rights; (void)operations;
        return (fileargs_t *)0x1;
    }
    static inline int       fileargs_open(fileargs_t *fa, const char *name)
        { (void)fa; return open(name, O_RDONLY); }
    static inline char *    fileargs_realpath(fileargs_t *fa, const char *name, char *resolved)
        { (void)fa; return realpath(name, resolved); }
    static inline FILE *    fileargs_fopen(fileargs_t *fa, const char *name, const char *mode)
        { (void)fa; return fopen(name, mode); }
    static inline void      fileargs_free(fileargs_t *fa) { (void)fa; }
    #define FA_OPEN          0x0001
    #define FA_REALPATH      0x0002
    #define FA_STAT          0x0004
    #define FA_LSTAT         0x0008
    #define FA_FSTATAT       0x0010
    #endif
    CFA_EOF
    # ── termcap stub ─────────────────────────────────────────────────
    # Minimal <termcap.h> with the legacy BSD termcap prototypes that
    # zsh, less, vi, … check for. yos has no terminfo database;
    # libyos_stubs ships no-op tgetent/tgoto/tputs/etc. so callers
    # link clean and fall back to raw-text output at runtime.
    cat > "$out/usr/include/termcap.h" <<'TC_EOF'
    #ifndef _TERMCAP_H_
    #define _TERMCAP_H_
    extern char  PC;
    extern char  *UP, *BC;
    extern short ospeed;
    int   tgetent(char *bp, const char *name);
    int   tgetnum(const char *id);
    int   tgetflag(const char *id);
    char *tgetstr(const char *id, char **area);
    char *tgoto(const char *cap, int col, int row);
    int   tputs(const char *str, int affcnt, int (*putc)(int));
    #endif
    TC_EOF
    cat > "$out/usr/include/casper/cap_net.h" <<'CNT_EOF'
    /* yos stub — Capsicum cap_net service. The real service routes
     * network calls through a privileged Casper helper that holds
     * limited rights. We don't sandbox; every helper falls through to
     * the underlying libc call. */
    #ifndef _CAP_NET_H_
    #define _CAP_NET_H_
    #include <sys/socket.h>
    #include <netdb.h>
    #include <stddef.h>
    #include <libcasper.h>
    typedef struct cap_net_limit cap_net_limit_t;
    #define CAPNET_NAME2ADDR    0x01
    #define CAPNET_ADDR2NAME    0x02
    #define CAPNET_CONNECT      0x04
    #define CAPNET_BIND         0x08
    #define CAPNET_CONNECTDNS   0x10
    static inline cap_net_limit_t *
    cap_net_limit_init(cap_channel_t *c, unsigned long mode)
    { (void)c; (void)mode; return (cap_net_limit_t *)0x1; }
    static inline int  cap_net_limit(cap_net_limit_t *l)
    { (void)l; return 0; }
    static inline void cap_net_limit_name2addr_family(cap_net_limit_t *l,
        const int *fams, size_t n)
    { (void)l; (void)fams; (void)n; }
    static inline void cap_net_free(cap_channel_t *c) { (void)c; }
    static inline int  cap_getaddrinfo(cap_channel_t *c, const char *host,
        const char *serv, const struct addrinfo *hints, struct addrinfo **res)
    { (void)c; return getaddrinfo(host, serv, hints, res); }
    static inline int  cap_getnameinfo(cap_channel_t *c, const struct sockaddr *sa,
        socklen_t sl, char *host, size_t hl, char *serv, size_t sl_, int flags)
    { (void)c; return getnameinfo(sa, sl, host, hl, serv, sl_, flags); }
    static inline int  cap_connect(cap_channel_t *c, int s, const struct sockaddr *a, socklen_t l)
    { (void)c; return connect(s, a, l); }
    static inline int  cap_bind(cap_channel_t *c, int s, const struct sockaddr *a, socklen_t l)
    { (void)c; return bind(s, a, l); }
    #endif
    CNT_EOF
    # NOTE: we deliberately do NOT install a sys/capsicum.h stub.
    # install_includes.py already copied the real one from
    # usr/src/sys/sys/capsicum.h, which carries the full set of
    # CAP_* constants (FreeBSD has dozens). Stubbing it would
    # under-deliver constants and break tools that compose them.
    #
    # The real header DECLARES cap_rights_init / cap_rights_set /
    # cap_rights_clear / cap_rights_is_set as `extern` — we let
    # those resolve as wasm imports against env.<name>. A stub at
    # the yos host side (hooks.yaml: stub:) returns success so
    # capsicum-aware code runs as if the kernel granted the rights.

    runHook postBuild
  '';

  # No standard install — buildPhase already wrote everything to $out.
  installPhase = "true";

  meta = with lib; {
    description = "wasm32 sysroot — curated FreeBSD-i386 headers + yos crt1";
    platforms = platforms.linux ++ platforms.darwin;
  };
}
