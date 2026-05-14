{ stdenv, lib, fetchurl, python3, toolchain, sysroot, zlib, openssl }:

# openssh — ssh / sshd / scp / sftp / ssh-keygen / ssh-agent / ssh-add,
# wasm32 static-linked against our zlib + openssl ports.
#
# Style follows upstream nixpkgs' openssh recipe (autoreconf-driven,
# explicit --without-X knobs to drop host-only integrations): no PAM,
# no SELinux, no Kerberos, no libedit, no PKCS11 — none of those have
# wasm32 ports here.

stdenv.mkDerivation rec {
  pname   = "openssh";
  version = "9.9p1";

  src = fetchurl {
    url    = "https://cdn.openbsd.org/pub/OpenBSD/OpenSSH/portable/openssh-${version}.tar.gz";
    sha256 = "sha256-s0P7zb/4fxWxmG5uFdbU/Jp9NgZr5rf7UHCHuo+WbAI=";
  };

  nativeBuildInputs = [ python3 toolchain ];
  buildInputs       = [ zlib openssl ];

  dontStrip    = true;
  dontPatchELF = true;
  dontFixup    = true;
  # nix's updateAutotoolsGnuConfigScriptsPhase replaces config.sub with
  # a newer upstream copy AFTER patchPhase — wiping our wasm32 shim.
  # Disable it; we've patched in wasm32 support ourselves.
  dontUpdateAutotoolsGnuConfigScripts = true;

  postPatch = ''
    # config.sub predates wasm32 — short-circuit so it accepts our
    # triple. Same trick zsh's port uses.
    if [ -f config.sub ]; then
      python3 - config.sub <<'PY'
    import sys, pathlib
    p = pathlib.Path(sys.argv[1])
    text = p.read_text()
    shim = """case $1 in
        wasm32-*-* | wasm64-*-*) echo "$1"; exit 0 ;;
    esac

    """
    marker = 'case $# in'
    i = text.find(marker)
    if i < 0: sys.exit('config.sub: marker not found')
    p.write_text(text[:i] + shim + text[i:])
    PY
    fi
    patchShebangs configure

    # Replace openbsd-compat/libressl-api-compat.c's get_iv/set_iv
    # bodies with openssl 3.x-compatible implementations using
    # OSSL_PARAM. The upstream file pokes the opaque ctx->iv field
    # directly, which the openssl 3.x build forbids. Header
    # contract is the same — only the bodies change.
    cat > openbsd-compat/libressl-api-compat.c <<'CSHIMEOF'
    /* libressl-api-compat.c — yos wasm32 port replacement.
     *
     * openssh's upstream shim provides EVP_CIPHER_CTX_get_iv /
     * EVP_CIPHER_CTX_set_iv by reaching into the opaque
     * evp_cipher_ctx_st struct. That works on libressl + openssl
     * 1.1.x but not on openssl 3.x where the struct is no longer
     * a public type. Rewrite both using the OSSL_PARAM mechanism
     * which IS the public 3.x API for cipher IV access.
     *
     * get_iv prefers EVP_CIPHER_CTX_get_updated_iv (3.x native);
     * set_iv uses EVP_CIPHER_CTX_set_params + OSSL_CIPHER_PARAM_IV.
     */
    #include "includes.h"
    #ifdef WITH_OPENSSL
    # include <openssl/evp.h>
    # include <openssl/core_names.h>
    # include <openssl/params.h>
    # include <string.h>

    #ifndef HAVE_EVP_CIPHER_CTX_GET_IV
    int
    EVP_CIPHER_CTX_get_iv(EVP_CIPHER_CTX *ctx, unsigned char *iv, size_t len)
    {
        if (ctx == NULL) return 0;
        return EVP_CIPHER_CTX_get_updated_iv(ctx, iv, len);
    }
    #endif

    #ifndef HAVE_EVP_CIPHER_CTX_SET_IV
    int
    EVP_CIPHER_CTX_set_iv(EVP_CIPHER_CTX *ctx, const unsigned char *iv, size_t len)
    {
        OSSL_PARAM params[2];
        if (ctx == NULL) return 0;
        params[0] = OSSL_PARAM_construct_octet_string(
            OSSL_CIPHER_PARAM_IV, (void *)iv, len);
        params[1] = OSSL_PARAM_construct_end();
        return EVP_CIPHER_CTX_set_params(ctx, params);
    }
    #endif

    #endif /* WITH_OPENSSL */
    CSHIMEOF

    # dns.c assumes the libc <netdb.h> provides getrrsetbyname() +
    # ERRSET_* constants + struct rrsetinfo (BSD/Linux only). Our
    # FreeBSD sysroot doesn't carry them; openssh's compat shim
    # under openbsd-compat/getrrsetbyname.h does, but dns.c never
    # includes it. Inject the include unconditionally after
    # "includes.h" — the shim's header itself is #ifndef-guarded.
    python3 - dns.c <<'PY'
    import sys, pathlib
    p = pathlib.Path(sys.argv[1])
    text = p.read_text()
    if '#include "openbsd-compat/getrrsetbyname.h"' not in text:
        text = text.replace(
            '#include "includes.h"',
            '#include "includes.h"\n#include "openbsd-compat/getrrsetbyname.h"',
            1)
        p.write_text(text)
    PY

    # Strip every `#ifndef HAVE_*` guard from the openbsd-compat
    # headers. configure's link-only feature probes report most of
    # these functions present (the symbols exist as libyos_stubs
    # weak placeholders), but our FreeBSD sysroot doesn't declare
    # them — the guards then skip the declarations and every caller
    # in openssh hits "undeclared function". Forcing the guards
    # open makes the compat decls + macros (FMT_SCALED_STRSIZE,
    # ERRSET_*, struct rrsetinfo, …) always available; the
    # corresponding .c files in openbsd-compat/ still get linked
    # whether they're physically used or not, providing the
    # implementations.
    python3 - <<'PY'
    import pathlib, re
    # Granular patch: only unblock the `#ifndef HAVE_X` guards for
    # functions our FreeBSD sysroot is missing, where configure's
    # link probe picks up a libyos_stubs weak symbol and incorrectly
    # claims the function present.
    #
    # Leave the rest of the guards alone so we don't redeclare
    # functions FreeBSD already has (user_from_uid, strlcpy, …).
    MISSING_IN_FREEBSD = [
        "GETRRSETBYNAME",
        "FMT_SCALED",
        "SCAN_SCALED",
        "ARC4RANDOM_STIR",
        "FREEZERO",
        "RECALLOCARRAY",
        "REALLOCARRAY",
        "BCRYPT_PBKDF",
        "TIMINGSAFE_BCMP",
        "STRNVIS",
        "STRTONUM",
        "GETPEEREID",
        "BLF_H",
        "BCRYPT_PBKDF",
        "EXPLICIT_MEMSET",
        "ARC4RANDOM_BUF",   # in FreeBSD but openssh's variant is fine
    ]
    for h in [
        "openbsd-compat/getrrsetbyname.h",
        "openbsd-compat/openbsd-compat.h",
    ]:
        p = pathlib.Path(h)
        if not p.exists(): continue
        text = p.read_text()
        for sym in MISSING_IN_FREEBSD:
            text = re.sub(
                rf"^#ifndef HAVE_{sym}\s*$",
                f"#if 1 /* yos: {sym} missing in FreeBSD sysroot */",
                text, flags=re.MULTILINE)
        p.write_text(text)
    PY
  '';

  configurePlatforms = [ ];

  CC     = "${toolchain}/bin/wasm-clang";
  AR     = "llvm-ar";
  RANLIB = "llvm-ranlib";

  CFLAGS = lib.concatStringsSep " " [
    "-target wasm32-unknown-unknown"
    "-nostdlib" "-nostdinc"
    "--sysroot=${sysroot}"
    "-isystem ${sysroot}/usr/include"
    "-D__i386__=1" "-D__yos__=1" "-D__FreeBSD__=14" "-D_GNU_SOURCE"
    "-O2" "-fno-builtin" "-ffreestanding"
    "-I${zlib}/include"
    "-I${openssl}/include"
  ];

  LDFLAGS = lib.concatStringsSep " " [
    "-target wasm32-unknown-unknown"
    "-nostdlib"
    "-Wl,--no-entry"
    "-Wl,--allow-undefined"
    "-Wl,--export-all"
    "${sysroot}/usr/lib/crt1.o"
    "-L${sysroot}/usr/lib"
    "-L${zlib}/lib"
    "-L${openssl}/lib"
  ];

  LIBS = "-lssl -lcrypto -lz -lc -lyos_stubs";

  configurePhase = ''
    runHook preConfigure

    # nix's stdenv resets CC=gcc via cc-wrapper setup hooks AFTER the
    # derivation's CC= attribute was exported. Force the wasm
    # toolchain explicitly so autoconf doesn't pick gcc up.
    export CC=${toolchain}/bin/wasm-clang
    export AR=llvm-ar
    export RANLIB=llvm-ranlib
    export LD=${toolchain}/bin/wasm-clang

    # Pre-seed cross-compile answers so configure's runtime feature
    # probes don't try to execute wasm binaries on the host.
    cat > config.cache <<'CACHE_EOF'
    # Functions that don't exist in our FreeBSD sysroot — force NO so
    # openssh's openbsd-compat provides its own version.
    ac_cv_func_freezero=no
    ac_cv_func_recallocarray=no
    ac_cv_func_reallocarray=no
    ac_cv_func_getpeereid=no
    ac_cv_func_getpeerucred=no
    ac_cv_func_strnvis=no
    ac_cv_func_strndup=yes
    ac_cv_func_explicit_memset=no
    ac_cv_func_setresuid=no
    ac_cv_func_setresgid=no
    ac_cv_func_setreuid=yes
    ac_cv_func_setregid=yes
    ac_cv_func_pledge=no
    ac_cv_func_unveil=no
    ac_cv_func_pledge=no
    ac_cv_func_endgrent=yes
    ac_cv_func_setlogin=no
    ac_cv_func_crypt=no
    ac_cv_search_crypt=no
    ac_cv_func_dirfd=yes
    ac_cv_func_fchmodat=yes
    ac_cv_func_fchownat=yes
    ac_cv_func_inet_aton=yes
    ac_cv_func_inet_ntoa=yes
    ac_cv_func_inet_ntop=yes
    ac_cv_func_innetgr=no
    ac_cv_func_login=no
    ac_cv_func_logout=no
    ac_cv_func_logwtmp=no
    ac_cv_func_getrrsetbyname=no
    ac_cv_func_mblen=yes
    ac_cv_func_memmem=yes
    ac_cv_func_mmap=yes
    ac_cv_func_openpty=no
    ac_cv_func__getpty=no
    ac_cv_func_bcrypt_pbkdf=no
    # OpenSSL 3.x: only EVP_CIPHER_CTX_get_updated_iv is in the
    # public headers. The legacy {get,set}_iv / iv / iv_noconst
    # still link (libcrypto.a keeps the symbols for ABI compat)
    # but no header declares them. Force NO so openssh's
    # libressl-api-compat.c provides its own — and we patch that
    # file in postPatch to use the OSSL_PARAM API on 3.x.
    ac_cv_func_EVP_CIPHER_CTX_iv=no
    ac_cv_func_EVP_CIPHER_CTX_iv_noconst=no
    ac_cv_func_EVP_CIPHER_CTX_get_iv=no
    ac_cv_func_EVP_CIPHER_CTX_set_iv=no
    ac_cv_func_EVP_CIPHER_CTX_get_updated_iv=yes
    # Present in FreeBSD sysroot.
    ac_cv_func_arc4random=yes
    ac_cv_func_arc4random_buf=yes
    ac_cv_func_arc4random_uniform=yes
    ac_cv_func_explicit_bzero=yes
    ac_cv_func_timingsafe_bcmp=yes
    ac_cv_func_clock_gettime=yes
    ac_cv_func_strtonum=yes
    ac_cv_func_strtoull=yes
    ac_cv_func_strtoll=yes
    ac_cv_func_setrlimit=yes
    ac_cv_func_pselect=yes
    ac_cv_func_ppoll=yes
    ac_cv_have_broken_snprintf=no
    ac_cv_have_decl_writev=yes
    ac_cv_have_decl_readv=yes
    ac_cv_have_decl_AI_NUMERICSERV=yes
    ac_cv_type_RETSIGTYPE=void
    CACHE_EOF

    ./configure \
        --host=wasm32-unknown-unknown \
        --prefix=$out \
        --cache-file=config.cache \
        --with-ssl-dir=${openssl} \
        --with-zlib=${zlib} \
        --without-pam --without-selinux --without-libedit \
        --without-libcrypt --without-ldns --without-kerberos5 \
        --without-shadow --without-audit --without-sandbox \
        --disable-utmp --disable-wtmp --disable-lastlog \
        --disable-pututline --disable-pututxline --disable-strip \
        --without-privsep-user
    runHook postConfigure
  '';

  buildPhase = ''
    runHook preBuild
    make -j$NIX_BUILD_CORES ssh sshd scp sftp ssh-keygen ssh-agent ssh-add
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    mkdir -p $out/bin $out/libexec
    for tool in ssh sshd scp sftp ssh-keygen ssh-agent ssh-add; do
      [ -f $tool ] || continue
      # Asyncify-instrument every binary — openssh definitely fork/execs.
      wasm-opt --asyncify -O2 $tool -o $out/libexec/$tool
      cat > $out/bin/$tool <<RUNNER_EOF
    #!/usr/bin/env bash
    exec yos $out/libexec/$tool "\$@"
    RUNNER_EOF
      chmod +x $out/bin/$tool
    done
    cat > $out/manifest.txt <<EOF
    name=openssh
    version=${version}
    prefix=$out
    deps=zlib openssl
    EOF
    runHook postInstall
  '';

  meta = with lib; {
    description = "OpenSSH wasm32 port for yos";
    homepage    = "https://www.openssh.com";
    license     = licenses.bsd2;
    platforms   = platforms.linux ++ platforms.darwin;
  };
}
