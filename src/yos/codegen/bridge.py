#!/usr/bin/env python3
"""Emit per-function bridge wrappers from analyse-report.yaml.

Each bridge wrapper is a host-side C function that:

  1. Receives the wasm-ABI representation of the guest's call args
     (i32 for int/long/pointer, i64 for long long, etc.)
  2. Translates any wasm-pointer offset to a host pointer
     (`ctx->memory + offset`)
  3. Widens/narrows scalars where guest and host C types differ in
     width (i.e. mechanical deltas)
  4. Calls the host libc function
  5. Maps return value back to the wasm ABI; remaps errno via
     yos_remap_errno_h2g when the host signalled an error

Scope of THIS iteration:
  - Emit real bridges for `compatible` functions whose arg types are
    "simple" (scalar / void* / pointer-to-builtin / pointer-to-void).
    These cover most basic POSIX surface (read, write, close, dup,
    chmod, getpid, …).
  - Emit `// TODO` stub for everything else (compatible-but-complex,
    mechanical, needs_policy). They return -ENOSYS at runtime so the
    guest gets a clean error if it hits one.

A full converter for `mechanical` (per-field struct walking) is a
follow-on — needs the type renderer to know how to lay out guest
structs in linear memory and walk them field by field.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import yaml


# ─── Type-renderer helpers ───────────────────────────────────────────

def _resolve(t: dict, types: dict) -> dict | None:
    """Walk through typedefs (we already forward at extract time, so
    this mostly handles defensive cases)."""
    seen = set()
    while t is not None and t.get('kind') == 'typedef':
        uid = t.get('type_uid')
        if not uid or uid in seen:
            break
        seen.add(uid)
        t = types.get(uid)
    return t


def _is_void_pointer(t: dict, types: dict) -> bool:
    if not t or t.get('kind') != 'pointer':
        return False
    pointee = _resolve(types.get(t.get('pointee_uid')), types)
    return pointee is not None and pointee.get('kind') == 'void'


def _is_const_char_pointer(t: dict, types: dict) -> bool:
    """Detect `const char *` (string args)."""
    if not t or t.get('kind') != 'pointer':
        return False
    pointee = _resolve(types.get(t.get('pointee_uid')), types)
    if not pointee or pointee.get('kind') != 'builtin':
        return False
    name = (pointee.get('name') or '').lower()
    return 'char' in name and t.get('pointee_is_const', False)


def _wasm_type(t: dict, types: dict) -> str:
    """Render a guest type as the wasm-ABI C type the bridge receives.
    Returns None if we can't render it cleanly (caller then falls
    back to a TODO stub)."""
    t = _resolve(t, types)
    if t is None:
        return None
    k = t.get('kind')
    if k == 'void':
        return 'void'
    if k == 'pointer':
        # Every wasm32 pointer is a u32 offset into linear memory.
        return 'uint32_t'
    if k == 'builtin':
        size = t.get('size')
        name = (t.get('name') or '')
        # Float types FIRST — they're 4 / 8 bytes too, so the
        # size-based int branches would otherwise swallow them.
        if name == 'float':
            return 'float'
        if name in ('double', 'long double'):
            return 'double'
        if size == 1:
            return 'int8_t' if 'unsigned' not in name and 'char' not in name else 'uint8_t'
        if size == 2:
            return 'uint16_t' if 'unsigned' in name else 'int16_t'
        if size == 4:
            return 'uint32_t' if 'unsigned' in name else 'int32_t'
        if size == 8:
            return 'uint64_t' if 'unsigned' in name else 'int64_t'
        return None
    if k == 'enum':
        return 'int32_t'
    # struct / union by value: not supported in this iteration.
    return None


def _host_type(t: dict, types: dict) -> str | None:
    """Render the host C type as it appears in the libc declaration.
    Used to construct the cast at the host-call site."""
    t = _resolve(t, types)
    if t is None:
        return None
    k = t.get('kind')
    if k == 'void':
        return 'void'
    if k == 'pointer':
        pointee = _resolve(types.get(t.get('pointee_uid')), types)
        if pointee is None:
            return None
        if pointee.get('kind') == 'void':
            return 'void *'
        sub = _host_type(pointee, types)
        if sub is None:
            return None
        return sub + ' *'
    if k == 'builtin':
        # Use the canonical clang spelling — this is what host libc
        # already declared and what we want at the call site.
        return t.get('name') or None
    if k == 'enum':
        return 'int'
    return None


def _guest_type(t: dict, types: dict) -> str | None:
    """Render the guest-side C type spelling for the FreeBSD
    declaration in the auto-generated `yos_imports.h`. We use the
    *canonical* spelling (e.g. `int` instead of `pid_t`, `long long`
    instead of `off_t`) — the FreeBSD typedefs themselves are
    already in the FreeBSD headers, and clang merges
    redeclarations canonical-typewise, so canonical spellings are
    enough to attach the import attributes correctly. Returns None
    if the type can't be rendered (struct by value, function-
    pointer, etc.).
    """
    t = _resolve(t, types)
    if t is None:
        return None
    k = t.get('kind')
    if k == 'void':
        return 'void'
    if k == 'pointer':
        pointee = _resolve(types.get(t.get('pointee_uid')), types)
        if pointee is None:
            return None
        # Suppress our `const` qualifier if the pointee already carries
        # one — extract.py records is_const on builtins (so the name
        # may already be the bare type, but the type registry's flag
        # tells us "this is const-qualified"). Either source means
        # "pointer-to-const-T"; emitting both yields the famous
        # `const char const *` warning.
        is_const = bool(t.get('pointee_is_const')) or bool(pointee.get('is_const'))
        prefix = 'const ' if is_const else ''
        if pointee.get('kind') == 'void':
            return f'{prefix}void *'
        sub = _guest_type(pointee, types)
        if sub is None:
            return None
        return f'{prefix}{sub} *'
    if k == 'builtin':
        # Strip the leading 'const ' if clang baked it into the name —
        # the const qualifier is redundantly carried by `is_const` and
        # the surrounding pointer's `pointee_is_const`. Letting the
        # caller add it once avoids `const const char *`.
        name = t.get('name') or ''
        if name.startswith('const '):
            name = name[len('const '):]
        return name or None
    if k == 'enum':
        return 'int'
    return None


# ─── Bridge emitter ──────────────────────────────────────────────────

def _bridge_arg_decl(name: str, idx: int, t: dict, types: dict) -> str | None:
    """Emit the wasm-side function-arg declaration. Returns None on
    types we don't yet render."""
    wt = _wasm_type(t, types)
    if wt is None:
        return None
    return f'{wt} {name or f"a{idx}"}'


def _arg_translation(name: str, idx: int, gt: dict, ht: dict, types: dict) -> tuple[str, str] | None:
    """For one function arg, return (local_setup, host_call_expr).

    local_setup is C lines that prepare a local of the host type;
    host_call_expr is what we pass to the host call.

    Returns None if we can't bridge this arg cleanly.
    """
    gt = _resolve(gt, types)
    ht = _resolve(ht, types)
    var = name or f'a{idx}'
    if gt is None or ht is None:
        return None
    gk, hk = gt.get('kind'), ht.get('kind')

    # void / void — shouldn't appear as an arg, but be defensive.
    if gk == 'void' and hk == 'void':
        return ('', '')

    # pointer args: translate wasm offset to host pointer.
    if gk == 'pointer' and hk == 'pointer':
        host_ptr_type = _host_type(ht, types) or 'void *'
        # Accept any guest pointer of the same level as the host's,
        # cast at the call site. Layout compatibility was already
        # confirmed by the comparator; we trust it here.
        setup = f'    {host_ptr_type} {var}_h = ({host_ptr_type})(ctx->memory + {var});'
        return (setup, f'{var}_h')

    # builtin / builtin: width-only conversion via cast at the call.
    if gk == 'builtin' and hk == 'builtin':
        host_t = _host_type(ht, types) or 'int'
        return ('', f'({host_t}){var}')

    # enum → int / vice versa.
    if gk == 'enum' and hk in ('builtin', 'enum'):
        return ('', f'({_host_type(ht, types) or "int"}){var}')
    if hk == 'enum' and gk in ('builtin', 'enum'):
        return ('', f'(int){var}')

    return None


_WASM_TYPE_TO_SIG = {
    'void':     'v',
    'int8_t':   'i', 'uint8_t':  'i',
    'int16_t':  'i', 'uint16_t': 'i',
    'int32_t':  'i', 'uint32_t': 'i',
    'int64_t':  'I', 'uint64_t': 'I',
    'float':    'f',
    'double':   'F',
}


def _sig_char_for_wasm_type(wt: str | None) -> str:
    """Map a rendered wasm-ABI C type to a wasm3 link-signature char.
    Defaults to 'i' (any unknown 32-bit thing — typically a pointer
    we render as uint32_t)."""
    return _WASM_TYPE_TO_SIG.get(wt or '', 'i')


def _emit_bridge(name: str, gf: dict, hf: dict, gtypes: dict, htypes: dict,
                 has_public_header: bool = True) -> tuple[str, str]:
    """Emit (declaration, definition) for one function's bridge. On
    failure returns (decl, stub_def) so the bridge still compiles but
    returns -ENOSYS at runtime."""
    # Wasm-side signature
    arg_decls = ['struct yos_exec_ctx *ctx']
    setups: list[str] = []
    call_args: list[str] = []
    can_emit = has_public_header

    g_args = gf.get('args', [])
    h_args = hf.get('args', [])
    for i, (ga, ha) in enumerate(zip(g_args, h_args)):
        gt = gtypes.get(ga['type_uid']);  ht = htypes.get(ha['type_uid'])
        decl = _bridge_arg_decl(ga.get('name'), i, gt, gtypes)
        tr   = _arg_translation(ga.get('name'), i, gt, ht, gtypes if False else gtypes, )
        # ↑ pass gtypes for resolve; host uses htypes — fix:
        tr = _arg_translation_full(ga.get('name'), i, gt, ht, gtypes, htypes)
        if decl is None or tr is None:
            can_emit = False
            break
        arg_decls.append(decl)
        if tr[0]:
            setups.append(tr[0])
        call_args.append(tr[1])

    # Variadic: clang's wasm32 ABI adds an implicit `i32 va_list_ptr`
    # at the end of the call. We accept it as an extra unused parameter
    # in the bridge — the variadic args themselves are ignored for now
    # (the body calls host libc with only the fixed args). Functions
    # that genuinely need varargs (printf family) live under
    # `variadic:` in hooks.yaml and have hand-written bodies.
    if gf.get('variadic'):
        arg_decls.append(f'uint32_t _va_ptr')
        # Host libc fns are also variadic at this level; clang's host
        # ABI passes varargs differently but since we don't expand any
        # varargs at the host call here, just don't add to call_args.

    # Return rendering
    g_ret = gtypes.get(gf['ret']);  h_ret = htypes.get(hf['ret'])
    wret  = _wasm_type(g_ret, gtypes)
    hret  = _host_type(h_ret, htypes)
    if wret is None or hret is None:
        can_emit = False

    # Functions that return a pointer need result translation: the
    # host returns a host pointer; the guest expects a wasm offset.
    # The common case is "return aliases first ptr arg" (memset, memcpy,
    # memmove, strcpy, strncpy, strcat, strncat, …) — for those we can
    # just return the first arg's wasm offset unchanged. The next
    # common case is "result is a pointer INTO a string arg" (strchr,
    # strrchr, memchr, strstr) — we compute the offset by subtracting
    # the input buffer's host base. Anything else (strdup-style new
    # allocations) genuinely needs an allocator and stays a stub.
    g_ret_resolved = _resolve(g_ret, gtypes)
    ret_is_ptr = bool(g_ret_resolved and g_ret_resolved.get('kind') == 'pointer')

    # Do the first arg(s) include a pointer? If so, decide which case.
    first_ptr_arg = -1
    for i, ga in enumerate(g_args):
        gt = _resolve(gtypes.get(ga['type_uid']), gtypes)
        if gt and gt.get('kind') == 'pointer':
            first_ptr_arg = i
            break

    # Functions whose return is the first ptr arg unchanged.
    RET_IS_DST = {
        'memset', 'memcpy', 'memmove', 'strcpy', 'strncpy',
        'strcat', 'strncat', 'mempcpy', 'stpcpy', 'stpncpy',
    }
    # Functions whose return is a pointer INTO the first ptr arg.
    RET_OFFSET_INTO_FIRST = {
        'strchr', 'strrchr', 'memchr', 'strstr', 'strpbrk',
        'strcasestr', 'memrchr',
    }
    ret_kind = None
    if ret_is_ptr and first_ptr_arg >= 0:
        if name in RET_IS_DST:
            ret_kind = 'first_arg_alias'
        elif name in RET_OFFSET_INTO_FIRST:
            ret_kind = 'offset_into_first'
    if ret_is_ptr and ret_kind is None:
        can_emit = False

    decl = f'{wret or "int32_t"} yos_{name}({", ".join(arg_decls)});'

    if not can_emit:
        # If we can't render the return type, fall back to int32_t —
        # the wasm-side caller treats it as an i32. Better than
        # leaving the function with no return at all.
        eff_wret = wret if (wret and wret != 'void') else 'int32_t'
        # For pointer returns, NULL (0) is far safer than -38 — guest
        # libc treats NULL as "fn returned nothing useful" and most
        # call sites cope (getenv, setlocale, fopen, …); -38 looks like
        # a valid pointer to the guest and crashes the next deref.
        ret_lit = '0' if ret_is_ptr else '(-38)'
        body = (
            f'{eff_wret} yos_{name}({", ".join(arg_decls)}) {{\n'
            f'    /* TODO: complex arg/return types — extend bridge.py to render. */\n'
            f'    (void)ctx;\n'
            f'    return ({eff_wret}){ret_lit};\n'
            f'}}'
        )
        # Make the declaration match.
        decl = f'{eff_wret} yos_{name}({", ".join(arg_decls)});'
        return decl, body

    # Compose the host-call expression with optional return-value
    # narrowing and errno remap.
    call = f'{name}({", ".join(call_args)})'
    if hret == 'void':
        body = (
            f'void yos_{name}({", ".join(arg_decls)}) {{\n'
            + ('\n'.join(setups) + '\n' if setups else '')
            + f'    (void)ctx;\n'
            + f'    {call};\n'
            + f'}}'
        )
        return f'void yos_{name}({", ".join(arg_decls)});', body

    body_lines = []
    body_lines.append(f'{wret} yos_{name}({", ".join(arg_decls)}) {{')
    body_lines.append('    (void)ctx;')
    body_lines.extend(setups)
    body_lines.append(f'    {hret} _r = {call};')
    if ret_kind == 'first_arg_alias':
        # memset/memcpy/strcpy/...: return is the first ptr arg as
        # passed in. Just return that argument's wasm offset.
        first_arg_var = arg_decls[1 + first_ptr_arg].split()[-1]
        body_lines.append(f'    (void)_r;')
        body_lines.append(f'    return {first_arg_var};')
    elif ret_kind == 'offset_into_first':
        # strchr/memchr/strstr/...: result is a pointer into the first
        # ptr arg's buffer. Return NULL → 0; else compute the offset
        # within the input and add to its wasm offset.
        first_arg_var = arg_decls[1 + first_ptr_arg].split()[-1]
        # Local set up by _arg_translation_full as <name>_h.
        host_first = f'{first_arg_var}_h'
        body_lines.append(f'    if (!_r) return 0;')
        body_lines.append(f'    return (uint32_t)({first_arg_var} + '
                          f'((const char *)_r - (const char *){host_first}));')
    else:
        # If host return is signed and could indicate -1 error → remap errno.
        if any(b in (hret or '') for b in ('int', 'long', 'ssize_t', 'off_t', 'pid_t')):
            body_lines.append('    if (_r < 0) {')
            body_lines.append('        extern int yos_remap_errno_h2g(int);')
            body_lines.append('        extern int errno;')
            body_lines.append('        return ({wret})(-yos_remap_errno_h2g(errno));'.format(wret=wret))
            body_lines.append('    }')
        body_lines.append(f'    return ({wret})_r;')
    body_lines.append('}')
    return decl, '\n'.join(body_lines)


def _arg_translation_full(name, idx, gt, ht, g_types, h_types):
    """Same as _arg_translation but with separate type registries.

    The earlier helper used a single types dict; bridges read guest
    types from one yaml and host types from another, so we accept
    both registries to resolve correctly.
    """
    gt = _resolve(gt, g_types)
    ht = _resolve(ht, h_types)
    var = name or f'a{idx}'
    if gt is None or ht is None:
        return None
    gk, hk = gt.get('kind'), ht.get('kind')
    if gk == 'void' and hk == 'void':
        return ('', '')
    if gk == 'pointer' and hk == 'pointer':
        host_ptr_type = _host_type(ht, h_types) or 'void *'
        setup = f'    {host_ptr_type} {var}_h = ({host_ptr_type})(ctx->memory + {var});'
        return (setup, f'{var}_h')
    if gk == 'builtin' and hk == 'builtin':
        host_t = _host_type(ht, h_types) or 'int'
        return ('', f'({host_t}){var}')
    if gk == 'enum' and hk in ('builtin', 'enum'):
        return ('', f'({_host_type(ht, h_types) or "int"}){var}')
    if hk == 'enum' and gk in ('builtin', 'enum'):
        return ('', f'(int){var}')
    return None


# ─── Top-level emit ──────────────────────────────────────────────────

_BRIDGE_PROLOGUE = '''\
/*
 * Auto-generated by build-tools/api-generate/bridge.py — DO NOT EDIT.
 *
 * Per-function bridge wrappers between the wasm guest's libc imports
 * and the host's libc. yos's runtime links these and binds each
 * `yos_<fn>` to its corresponding wasm import.
 *
 * The struct yos_exec_ctx contains at minimum a `uint8_t *memory`
 * pointing at the wasm linear-memory base — yos host code provides
 * the actual layout.
 */
'''


def _normalised_header(h: str) -> str | None:
    """Snapshot path → libc-style header name. Returns None for headers
    that have no clean public include (linux/ uapi, glibc bits, asm)."""
    if not h:
        return None
    if '/' in h:
        tail = h.split('/', 1)[1] if h[:3].startswith(('01-', '02-', '03-', '04-')) else h
    else:
        tail = h
    if tail.startswith(('bits/', 'asm/', 'asm-generic/', 'linux/',
                        'rpcsvc/', 'rpc/')):
        return None
    return tail


def _has_public_header(name: str, host_fns: dict) -> bool:
    """True if the host's declaration lives in a header we can include
    in user code. Functions buried inside kernel/uapi/glibc-internal
    headers can't be called by name from a normal .c file."""
    f = host_fns.get(name)
    if not f:
        return False
    return _normalised_header(f.get('header') or '') is not None


def _collect_includes(names, host_fns) -> list[str]:
    """Distinct list of #include paths needed to declare every host
    function we call. Reads `header:` tags written by extract.py.
    Strips the snapshot/path/ prefix so we end up with `<sys/stat.h>`,
    `<stdio.h>`, etc.
    """
    incs: set[str] = set()
    for n in names:
        f = host_fns.get(n)
        if not f:
            continue
        tail = _normalised_header(f.get('header') or '')
        if tail is None:
            continue
        incs.add(tail)
    return sorted(incs)


def _wasm_sig(name: str, gf: dict, gtypes: dict) -> tuple[str, list[str]] | None:
    """Compute the (ret_char, [arg_chars]) wasm3 link signature for a
    function, based on the guest declaration. Returns None if any type
    can't be rendered as a wasm-ABI scalar.

    For variadic functions (open, fcntl, ioctl, …), clang's wasm32 ABI
    adds an implicit `i32` (a pointer to the va_list staging area) to
    the call. We add it to the sig so the bridge matches what the
    wasm guest actually emits at the call site.
    """
    arg_chars: list[str] = []
    for ga in gf.get('args', []):
        wt = _wasm_type(gtypes.get(ga['type_uid']), gtypes)
        if wt is None:
            return None
        arg_chars.append(_sig_char_for_wasm_type(wt))
    if gf.get('variadic'):
        arg_chars.append('i')
    wret = _wasm_type(gtypes.get(gf['ret']), gtypes)
    rchar = _sig_char_for_wasm_type(wret) if wret is not None else 'i'
    return rchar, arg_chars


def _emit_m3_wrapper(name: str, ret_char: str, arg_chars: list[str]) -> str:
    """Emit the wasm3 raw-function wrapper that pops args off the m3
    stack, calls yos_<name>, and pushes the return."""
    # Map sig char -> the C type we use to pop the arg.
    pop_type = {'i': 'uint32_t', 'I': 'uint64_t', 'f': 'float', 'F': 'double'}

    lines = [
        f'static const void *m3w_{name}('
        'IM3Runtime runtime, IM3ImportContext _ctx, '
        'uint64_t *_sp, void *_mem)',
        '{',
        '    (void)_ctx; (void)_mem;',
        '    struct yos_exec_ctx *ctx = '
        '(struct yos_exec_ctx *)m3_GetUserData(runtime);',
    ]
    if ret_char != 'v':
        lines.append(f'    {pop_type[ret_char]} *raw_return = '
                     f'({pop_type[ret_char]}*)(_sp++);')

    arg_names: list[str] = []
    for i, c in enumerate(arg_chars):
        an = f'a{i}'
        lines.append(f'    {pop_type[c]} {an} = '
                     f'*({pop_type[c]}*)(_sp++);')
        arg_names.append(an)

    call = f'yos_{name}(ctx{("," if arg_names else "")} '
    call += ', '.join(arg_names) + ')'
    if ret_char == 'v':
        lines.append(f'    {call};')
    else:
        lines.append(f'    *raw_return = ({pop_type[ret_char]}){call};')
    lines.append('    return 0;  /* m3Err_none */')
    lines.append('}')
    return '\n'.join(lines)


def _emit_guest_imports_h(decls: list[tuple[str, str, list[str]]]) -> str:
    """Emit the guest-side supplemental header `yos_imports.h`.

    Each entry decorates a FreeBSD libc function with import
    attributes so clang -target wasm32 emits an `(import "env"
    "<name>")` for it. The C type spelling is canonical (FreeBSD
    typedefs live in FreeBSD's own headers — the redeclaration
    here merges with them). Only "compatible" passthrough
    functions are emitted: their FreeBSD wasm32 ABI is layout-
    identical to the host libc ABI, so no shape conversion runs
    in the bridge.
    """
    head = (
        '/*\n'
        ' * yos_imports.h — auto-generated by build-tools/api-generate/\n'
        ' *                bridge.py. DO NOT EDIT.\n'
        ' *\n'
        ' * One redeclaration per FreeBSD libc function whose ABI matches\n'
        ' * the host libc bit-for-bit ("compatible" in the analyser).\n'
        ' * `__attribute__((import_module("env"), import_name("X")))`\n'
        ' * tells clang -target wasm32 to emit the call as a wasm import.\n'
        ' * Include this header AFTER FreeBSD\'s normal headers; clang\n'
        ' * merges the redeclarations canonical-typewise and applies the\n'
        ' * import attributes to every call site.\n'
        ' */\n'
        '#ifndef YOS_IMPORTS_H\n'
        '#define YOS_IMPORTS_H\n\n'
        '#define YOS_IMP \\\n'
        '    __attribute__((import_module("env"), import_name(__func_name)))\n'
        '\n'
        '#ifdef __cplusplus\nextern "C" {\n#endif\n\n'
    )
    body: list[str] = []
    for name, ret_t, arg_ts in decls:
        # Per-decl import_name attribute (no macro indirection — keep the
        # generated source greppable).
        attr = (f'__attribute__((import_module("env"), '
                f'import_name("{name}")))')
        sig_args = ', '.join(arg_ts) if arg_ts else 'void'
        body.append(f'{attr}\n{ret_t} {name}({sig_args});')
    tail = (
        '\n\n#ifdef __cplusplus\n}\n#endif\n'
        '#endif /* YOS_IMPORTS_H */\n'
    )
    return head + '\n\n'.join(body) + tail


def _emit_link_imports(sigs: dict[str, tuple[str, list[str]]]) -> str:
    """Emit yos_link_imports(IM3Module) which calls m3_LinkRawFunction
    for every bridge. Tolerates "function not found" — guests don't
    have to import every libc symbol."""
    head = [
        '/* Hooked up by src/yos/yos-main.c. m3Err_functionLookupFailed',
        ' * means the wasm module simply didn\'t import this symbol —',
        ' * not a fatal condition for our usage. */',
        '/* m3Err_functionLookupFailed is declared by wasm3.h (already extern). */',
        '',
        'int yos_brg_link_imports(IM3Module mod)',
        '{',
        '    M3Result r = NULL;',
    ]
    body = []
    for name, (rchar, args) in sorted(sigs.items()):
        sig = f'{rchar}({"".join(args)})'
        body.append(
            f'    r = m3_LinkRawFunction(mod, "env", "{name}", '
            f'"{sig}", &m3w_{name});\n'
            f'    if (r && r != m3Err_functionLookupFailed) {{\n'
            f'        fprintf(stderr, "yos: link {name}: %s\\n", r);\n'
            f'        n_failed++;\n'
            f'    }}'
        )
    # Sig-mismatch on a single fn shouldn't take out the whole link
    # pass — skip it (the unresolved-stub catches it later) and keep
    # going. Returning 0 lets the wasm load even when a few bridges
    # don't bind. `n_failed` is just a tally for the "yos:" log.
    tail = [
        '    if (n_failed) fprintf(stderr, '
            '"yos: %d bridge(s) failed to link (see lines above)\\n", '
            'n_failed);',
        '    return 0;',
        '}',
    ]
    head_with_count = head + ['    int n_failed = 0;']
    return '\n'.join(head_with_count + body + tail)


def emit_bridge(analyse: dict, guest_api: dict, host_api: dict,
                hooks: dict[str, str] | None = None) -> tuple[str, str, dict, str]:
    hooks = hooks or {}
    g_fns, h_fns = guest_api.get('functions', {}), host_api.get('functions', {})
    g_types, h_types = guest_api.get('types', {}), host_api.get('types', {})

    # `compatible` (types match exactly) and `mechanical` (types differ
    # only in safe widen/narrow/scalar casts) are both auto-bridgeable
    # by _emit_bridge — the cast at the host call site handles the
    # mechanical deltas. needs_policy / unsupported / variadics still
    # have to fall through as stubs.
    candidates = (
        list(analyse.get('compatible') or [])
        + list((analyse.get('mechanical') or {}).keys())
    )
    stub_only  = (
        list((analyse.get('needs_policy') or {}).keys())
        + list((analyse.get('unsupported') or {}).keys())
        + list(analyse.get('variadic_skipped') or [])
    )

    # Anything in hooks.yaml that is NOT already in the analyse-report
    # (e.g. fork/getpid/pthread_create are needs_policy on size, but
    # we still want bridges for them via custom_proc/custom_pthread).
    # Add them to candidates so emit_bridge processes them.
    hooked_only = [n for n in hooks
                   if n not in candidates and n not in stub_only]
    stub_only += hooked_only

    decls: list[str] = []
    defs:  list[str] = []
    sigs:  dict[str, tuple[str, list[str]]] = {}  # name -> (ret_char, arg_chars)
    guest_decls: list[tuple[str, str, list[str]]] = []
    counts = {
        'passthrough_real': 0, 'passthrough_stub': 0,
        'custom_routed':    0,
        'enosys_stub':      0,
        'variadic_skipped': 0, 'runtime_skipped': 0,
        'skipped':          0, 'guest_decls':     0,
    }

    def _wargs_decl(gf):
        """Render the wasm-ABI parameter declarations for a function.
        Returns (list_of_decls, ok).  Variadic functions get an extra
        `uint32_t _va_ptr` to match clang's wasm32 ABI lowering."""
        out = []
        for i, ga in enumerate(gf.get('args', [])):
            gt = g_types.get(ga['type_uid'])
            d = _bridge_arg_decl(ga.get('name'), i, gt, g_types)
            if d is None:
                return None, False
            out.append(d)
        if gf.get('variadic'):
            out.append('uint32_t _va_ptr')
        return out, True

    for name in sorted(set(candidates) | set(stub_only)):
        category = hooks.get(name, 'passthrough')

        # ── Skipped entirely: codegen emits no body, no wrapper, no
        # link entry. Hand-written elsewhere. ─────────────────────────
        if category == 'runtime_owned':
            counts['runtime_skipped'] += 1
            continue
        if category == 'variadic':
            counts['variadic_skipped'] += 1
            continue

        gf, hf = g_fns.get(name), h_fns.get(name)
        if not gf:
            counts['skipped'] += 1
            continue
        # Host fn isn't strictly required for routed (custom_*) bridges,
        # but is required for passthrough bodies that call host libc.
        if category == 'passthrough' and not hf:
            counts['skipped'] += 1
            continue

        wsig = _wasm_sig(name, gf, g_types)
        if wsig is None:
            counts['skipped'] += 1
            continue

        # Always emit a guest-side import-decorated decl so apps can
        # call the function and clang turns it into an env import —
        # except for variadic / runtime_owned (already handled above).
        ret_spelling = _guest_type(g_types.get(gf['ret']), g_types)
        arg_spellings = []
        guest_ok = ret_spelling is not None
        for ga in gf.get('args', []):
            spell = _guest_type(g_types.get(ga['type_uid']), g_types)
            if spell is None:
                guest_ok = False
                break
            arg_spellings.append(spell)
        if guest_ok:
            guest_decls.append((name, ret_spelling, arg_spellings))
            counts['guest_decls'] += 1

        # ── Custom routing — bridge.py emits ONLY a forward decl + the
        # m3w wrapper. The yos_<name>(ctx, ...) body is in
        # src/yos/impl/<area>.c. ─────────────────────────────────────
        if category.startswith('custom_'):
            wargs, ok = _wargs_decl(gf)
            if not ok:
                counts['skipped'] += 1
                continue
            wret = _wasm_type(g_types.get(gf['ret']), g_types) or 'int32_t'
            decl = (f'extern {wret} yos_{name}(struct yos_exec_ctx *ctx'
                    f'{(", " + ", ".join(wargs)) if wargs else ""});')
            decls.append(decl)
            counts['custom_routed'] += 1
            sigs[name] = wsig
            continue

        # ── from_freebsd_src (Tier 2): bridge dispatches into the
        # sidecar libc-pure.wasm runtime. The body resolves the
        # sidecar function once on first call (cached) and forwards
        # via m3_CallV. Today only scalar signatures are supported;
        # pointer args need cross-runtime memory marshalling (TBD). ──
        if category == 'from_freebsd_src':
            # Refuse if any arg or return is a pointer — we don't yet
            # marshal across runtimes. Fall through to stub instead.
            has_ptr = any(c == 'i' and (
                spelling.endswith('*')
            ) for c, spelling in zip(wsig[1], arg_spellings))
            ret_is_ptr = (ret_spelling or '').rstrip().endswith('*')
            if has_ptr or ret_is_ptr:
                # Emit as -ENOSYS until cross-runtime pointer
                # marshalling lands.
                wargs, ok = _wargs_decl(gf)
                if not ok:
                    counts['skipped'] += 1
                    continue
                wret = _wasm_type(g_types.get(gf['ret']), g_types) or 'int32_t'
                sig  = (f'{wret} yos_{name}(struct yos_exec_ctx *ctx'
                        f'{(", " + ", ".join(wargs)) if wargs else ""})')
                decls.append(sig + ';')
                ret_line = (f'    return ({wret})(-38); /* -ENOSYS */\n'
                            if wret != 'void' else '')
                defs.append(
                    f'{sig} {{\n'
                    f'    /* {name}: from_freebsd_src needs pointer marshalling. */\n'
                    f'    (void)ctx;\n'
                    + ''.join(f'    (void){a.split()[-1]};\n' for a in wargs)
                    + ret_line
                    + f'}}'
                )
                counts['enosys_stub'] += 1
                sigs[name] = wsig
                continue
            # Scalar-only: emit yos_<name>(ctx, ...) that calls the
            # sidecar via the helpers in impl/tier2.h.
            wargs, ok = _wargs_decl(gf)
            if not ok:
                counts['skipped'] += 1
                continue
            wret = _wasm_type(g_types.get(gf['ret']), g_types) or 'int32_t'
            arg_names = [a.split()[-1] for a in wargs]
            sig  = (f'{wret} yos_{name}(struct yos_exec_ctx *ctx'
                    f'{(", " + ", ".join(wargs)) if wargs else ""})')
            decls.append(sig + ';')
            body_lines = [
                f'{sig} {{',
                f'    (void)ctx;',
                f'    static IM3Function _f;',
                f'    IM3Function f = yos_tier2_resolve_once(&_f, "{name}");',
                f'    if (!f) return ({wret})(-38);  /* -ENOSYS */',
            ]
            call_args = ', '.join(arg_names) if arg_names else ''
            if call_args:
                body_lines.append(f'    M3Result _r = m3_CallV(f, {call_args});')
            else:
                body_lines.append(f'    M3Result _r = m3_CallV(f);')
            body_lines.append(f'    if (_r) return ({wret})-1;')
            if wret != 'void':
                body_lines.append(f'    {wret} _out = 0;')
                body_lines.append(f'    m3_GetResultsV(f, &_out);')
                body_lines.append(f'    return _out;')
            body_lines.append('}')
            defs.append('\n'.join(body_lines))
            counts['from_freebsd_src'] = counts.get('from_freebsd_src', 0) + 1
            sigs[name] = wsig
            continue

        # ── Hand-marked stub: -ENOSYS body. ───────────────────────────
        if category == 'stub':
            wargs, ok = _wargs_decl(gf)
            if not ok:
                counts['skipped'] += 1
                continue
            wret = _wasm_type(g_types.get(gf['ret']), g_types) or 'int32_t'
            # Pointer returns get NULL, not -ENOSYS — guests deref the
            # result and crash on -38.
            g_ret_resolved2 = _resolve(g_types.get(gf['ret']), g_types)
            stub_lit = '0' if (g_ret_resolved2 and
                               g_ret_resolved2.get('kind') == 'pointer') else '(-38)'
            sig  = (f'{wret} yos_{name}(struct yos_exec_ctx *ctx'
                    f'{(", " + ", ".join(wargs)) if wargs else ""})')
            decls.append(sig + ';')
            ret_line = (f'    return ({wret}){stub_lit};\n'
                        if wret != 'void' else '')
            defs.append(
                f'{sig} {{\n'
                f'    /* {name}: hooks.yaml -> stub (Linux-only or unportable). */\n'
                f'    (void)ctx;\n'
                + ''.join(f'    (void){a.split()[-1]};\n' for a in wargs)
                + ret_line
                + f'}}'
            )
            counts['enosys_stub'] += 1
            sigs[name] = wsig
            continue

        # ── struct_convert: TODO. Falls through to passthrough for now;
        # the wasm32<->host64 conversion will land in a follow-up. ────
        # ── Passthrough (or struct_convert TODO): existing emitter
        # writes a yos_<name>(ctx, ...) body that calls host libc. ────
        if not hf:
            counts['skipped'] += 1
            continue
        pub = _has_public_header(name, h_fns)
        decl, body = _emit_bridge(name, gf, hf, g_types, h_types,
                                  has_public_header=pub)
        decls.append(decl)
        defs.append(body)
        if 'TODO' in body:
            counts['passthrough_stub'] += 1
        else:
            counts['passthrough_real'] += 1
        sigs[name] = wsig

    # Collect the host-libc headers we need to include so the bridge
    # bodies can call the real functions by name. Pulled from the
    # `header:` tags extract.py recorded.
    host_call_names = list(analyse.get('compatible') or []) + stub_only
    host_includes = _collect_includes(host_call_names, h_fns)
    # Plus a few canonical headers our bodies always reach for.
    for fixed in ('errno.h', 'stdint.h', 'unistd.h', 'stdlib.h',
                  'stdio.h', 'string.h', 'time.h', 'signal.h',
                  'sys/types.h'):
        if fixed not in host_includes:
            host_includes.append(fixed)
    host_includes.sort()

    # Per-function wasm3 raw wrappers + the linker.
    wrappers = [_emit_m3_wrapper(name, rchar, args)
                for name, (rchar, args) in sorted(sigs.items())]
    linker = _emit_link_imports(sigs)

    h = (_BRIDGE_PROLOGUE
         + '#ifndef YOS_BRIDGE_H\n#define YOS_BRIDGE_H\n\n#include <stdint.h>\n'
           '\n/* The full struct is defined in src/yos/yos-types.h. Bridges\n'
           ' * only ever touch ctx->wasm_memory, so the forward decl plus\n'
           ' * that field via the public typedef is enough at the call\n'
           ' * site. Linking pulls in the real definition. */\n'
           'struct yos_exec_ctx;\n\n'
           '/* Forward decl of wasm3 module handle; full type comes from wasm3.h. */\n'
           'struct M3Module;\n'
           'typedef struct M3Module *IM3Module;\n\n'
           '#ifdef __cplusplus\nextern "C" {\n#endif\n\n'
           '/* Bind every yos_<fn> bridge as a wasm import in module `env`.\n'
           ' * Tolerates missing imports; returns -1 only on a real wasm3\n'
           ' * link error. Renamed from yos_link_imports to avoid clashing\n'
           ' * with the runtime\'s own linker in main.c. */\n'
           'int yos_brg_link_imports(IM3Module mod);\n\n'
         + '\n'.join(decls)
         + '\n\n#ifdef __cplusplus\n}\n#endif\n'
           '#endif /* YOS_BRIDGE_H */\n')

    include_block = '\n'.join(f'#include <{p}>' for p in host_includes)
    c = (_BRIDGE_PROLOGUE
         + '#include "yos_bridge.h"\n'
         + '#include "yos/types.h"  /* full struct yos_exec_ctx for ctx->memory */\n'
         + '#include "wasm3.h"     /* m3ApiRawFunction, m3_LinkRawFunction, ... */\n'
         + '#include "impl/tier2.h" /* yos_tier2_resolve_once for from_freebsd_src */\n'
         + include_block + '\n'
         + 'extern int yos_remap_errno_h2g(int);\n\n'
         + '/* ---- bridge bodies (call host libc) ---- */\n\n'
         + '\n\n'.join(defs)
         + '\n\n/* ---- wasm3 raw-function wrappers ---- */\n\n'
         + '\n\n'.join(wrappers)
         + '\n\n/* ---- import linker ---- */\n\n'
         + linker
         + '\n')

    guest_h = _emit_guest_imports_h(sorted(guest_decls,
                                          key=lambda x: x[0]))
    return h, c, counts, guest_h


def _load_hooks(path: Path | None) -> dict[str, str]:
    """Read hooks.yaml and produce a flat name -> category map.
    Default category for any name not listed is 'passthrough'."""
    if path is None:
        return {}
    raw = yaml.safe_load(path.read_text()) or {}
    cat_map: dict[str, str] = {}
    list_cats = ('custom_proc', 'custom_pthread', 'custom_vfs',
                 'custom_mem', 'custom_alloc', 'custom_sig',
                 'variadic', 'stub', 'runtime_owned')
    for cat in list_cats:
        for name in raw.get(cat) or []:
            cat_map[name] = cat
    # struct_convert and from_freebsd_src use name -> {meta} mappings
    for name in (raw.get('struct_convert') or {}):
        cat_map[name] = 'struct_convert'
    for name in (raw.get('from_freebsd_src') or {}):
        cat_map[name] = 'from_freebsd_src'
    return cat_map


def main() -> int:
    p = argparse.ArgumentParser(description='Emit yos_bridge.{h,c} from analyse-report.yaml.')
    p.add_argument('--analyse',   required=True, type=Path)
    p.add_argument('--guest-api', required=True, type=Path)
    p.add_argument('--host-api',  required=True, type=Path)
    p.add_argument('--hooks',     required=False, type=Path)
    p.add_argument('--out-dir',   required=True, type=Path)
    args = p.parse_args()

    with args.analyse.open()   as f: analyse   = yaml.safe_load(f)
    with args.guest_api.open() as f: guest_api = yaml.safe_load(f)
    with args.host_api.open()  as f: host_api  = yaml.safe_load(f)
    hooks = _load_hooks(args.hooks)

    h, c, counts, guest_h = emit_bridge(analyse, guest_api, host_api, hooks)
    args.out_dir.mkdir(parents=True, exist_ok=True)
    (args.out_dir / 'yos_bridge.h').write_text(h)
    (args.out_dir / 'yos_bridge.c').write_text(c)
    (args.out_dir / 'yos_imports.h').write_text(guest_h)

    print(
        f'[bridge] passthrough={counts["passthrough_real"]}  '
        f'passthrough-stub={counts["passthrough_stub"]}  '
        f'custom-routed={counts["custom_routed"]}  '
        f'enosys-stub={counts["enosys_stub"]}  '
        f'variadic-skipped={counts["variadic_skipped"]}  '
        f'runtime-skipped={counts["runtime_skipped"]}  '
        f'skipped={counts["skipped"]}  '
        f'guest-imports-h={counts["guest_decls"]}',
        file=sys.stderr)
    print(f'         wrote yos_bridge.h, yos_bridge.c, yos_imports.h → {args.out_dir}',
          file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
