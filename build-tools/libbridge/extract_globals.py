#!/usr/bin/env python3
"""extract_globals.py — enumerate every writable file-scope global a
library exposes. Inputs are paired:

  --headers <h1.h> [h2.h …]    libclang walks these to collect
                                file-scope VarDecls (declarations
                                with type info, including
                                _Thread_local / __thread).
  --so <path-to-libfoo.so>      readelf-style symbol-table walk over
                                this binary to collect actual
                                STT_OBJECT defs and their section
                                (PROGBITS/.data vs NOBITS/.bss vs
                                TLS / .rodata).

We intersect the two: anything declared in headers AND defined in the
.so is an externally-visible global. Section tells us whether it's
mutable (.data/.bss/.tdata/.tbss) or constant (.rodata). _Thread_local
(declared) and TLS section (.tdata/.tbss) are independent signals — we
emit both so the policy generator can flag mismatches.

Output: YAML to stdout (or --out <file>):

  library: libpython3.12.so
  globals:
    - name: _PyRuntime
      header: cpython/internal/pycore_runtime.h
      type: struct _PyRuntimeState
      size: 31480
      section: .bss              # NOBITS — zero-initialized
      mutable: true
      threadlocal: false         # not __thread / _Thread_local
      hazard: shared             # per-ctx-isolation hazard class
    - name: _Py_NoneStruct
      header: object.h
      type: PyObject
      size: 16
      section: .data
      mutable: true              # technically; refcount mutates
      threadlocal: false
      hazard: immortal_singleton # shared but safe — immutable
    …

Hazard classifications:
  shared           — mutable, not thread-local; cross-guest leak risk
  shared_const     — in .rodata; safe (immutable)
  threadlocal      — in TLS section; per-host-thread, safe under
                     yos's fork=pthread model
  immortal_singleton — heuristic match: name in known-safe list
                     (_Py_NoneStruct, _Py_TrueStruct, small int cache)
  unknown          — defined in .so but no header decl found

Run from the repo root via uv:
  uv run python build-tools/libbridge/extract_globals.py \
      --so /usr/lib/x86_64-linux-gnu/libpython3.12.so.1.0 \
      --headers Python.h \
      --cflags -I/usr/include/python3.12 \
      --out /tmp/globals-libpython.yaml
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

import yaml

# Reuse the libclang config-and-preload dance from the existing
# extract.py so this script works under nix dev shells too.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent /
                       'src' / 'yos' / 'codegen'))
from extract import _preload_libstdcxx, _find_libclang  # noqa: E402
_preload_libstdcxx()
import clang.cindex
_so = _find_libclang()
if _so:
    clang.cindex.Config.set_library_file(_so)

from clang.cindex import CursorKind, TypeKind  # noqa: E402


# ─── Header-side: every file-scope VarDecl libclang sees ─────────────

def _is_at_tu_scope(cursor) -> bool:
    p = cursor.semantic_parent
    return p is not None and p.kind == CursorKind.TRANSLATION_UNIT


def _short_header(path: str) -> str:
    """Trim system-include prefixes for readable output."""
    for marker in ('usr/include/', 'include/', 'Headers/'):
        i = path.find(marker)
        if i >= 0:
            return path[i + len(marker):]
    return path


def _type_spelling(t) -> str:
    s = t.spelling or '?'
    # Drop "(unaligned)" etc. that clang sometimes adds.
    return s.replace(' ', ' ').strip()


def walk_headers(headers: list[str], cflags: list[str]) -> dict[str, dict]:
    """Build a synthetic TU including every header; return
    {name: {type, header, threadlocal_declared, is_const_declared, …}}."""
    src = ''.join(f'#include <{h}>\n' for h in headers)
    with tempfile.NamedTemporaryFile(mode='w', suffix='.c', delete=False) as f:
        f.write(src)
        tmp = f.name
    try:
        index = clang.cindex.Index.create()
        tu = index.parse(tmp, args=cflags,
                         options=(clang.cindex.TranslationUnit
                                  .PARSE_SKIP_FUNCTION_BODIES))
        # Surface clang errors so we don't silently miss declarations.
        errs = [d for d in tu.diagnostics
                if d.severity >= clang.cindex.Diagnostic.Error]
        for d in errs:
            print(f'  clang: {d}', file=sys.stderr)

        out: dict[str, dict] = {}
        for cur in tu.cursor.walk_preorder():
            if cur.kind != CursorKind.VAR_DECL:
                continue
            if not _is_at_tu_scope(cur):
                continue
            # Skip the synthetic top-file's own vars (there are none, but
            # be defensive).
            loc = cur.location
            if not loc or not loc.file:
                continue
            name = cur.spelling
            if not name:
                continue
            # `extern` declarations and definitions both interest us —
            # they tell us "this name is exposed as a TU-scope variable
            # in this header". Multiple decls of the same name: keep the
            # first one we see. The .so cross-check is authoritative for
            # whether it's actually defined.
            if name in out:
                continue

            ct = cur.type
            # Detect _Thread_local / __thread. libclang's
            # storage_class doesn't expose this directly; we read it
            # via the token stream (cheap on PARSE_SKIP_FUNCTION_BODIES).
            tokens = [t.spelling for t in cur.get_tokens()]
            threadlocal_declared = any(
                t in ('_Thread_local', '__thread', 'thread_local')
                for t in tokens[:8])
            is_const_declared = ct.is_const_qualified()

            out[name] = {
                'type': _type_spelling(ct),
                'header': _short_header(str(loc.file)),
                'threadlocal_declared': threadlocal_declared,
                'is_const_declared': is_const_declared,
            }
        return out
    finally:
        os.unlink(tmp)


# ─── Binary-format dispatch (ELF vs Mach-O) ───────────────────────────

def _detect_format(path: str) -> str:
    """Read the magic to decide ELF vs Mach-O. Returns 'elf', 'macho',
    or '?' (caller errors out). Mach-O has several flavours
    (32/64-bit, fat universal); we treat all of them as 'macho' since
    llvm-nm handles the slicing for us."""
    with open(path, 'rb') as f:
        m = f.read(4)
    if m[:4] == b'\x7fELF':
        return 'elf'
    if m in (b'\xfe\xed\xfa\xce',  # MH_MAGIC (32-bit BE)
             b'\xce\xfa\xed\xfe',  # MH_CIGAM
             b'\xfe\xed\xfa\xcf',  # MH_MAGIC_64
             b'\xcf\xfa\xed\xfe',  # MH_CIGAM_64
             b'\xca\xfe\xba\xbe',  # FAT_MAGIC (universal)
             b'\xbe\xba\xfe\xca'): # FAT_CIGAM
        return 'macho'
    return '?'


# ─── ELF readers (Linux / FreeBSD glibc-class) ────────────────────────

def _readelf_sections(so: str) -> dict[int, dict]:
    """Section index → {name, type, flags, addr, size}.
    Indexes section by header number (Idx column) so symbols' Ndx can
    be resolved to a name + type. Used to distinguish .data (PROGBITS)
    from .bss (NOBITS) from .tdata/.tbss (PROGBITS+TLS / NOBITS+TLS)
    from .rodata (PROGBITS, readonly)."""
    out: dict[int, dict] = {}
    p = subprocess.run(['readelf', '-WS', so],
                       capture_output=True, text=True, check=True)
    # Lines look like:
    #   [ 6] .text   PROGBITS  0000000000020000 020000 1234 00 AX  0   0 16
    for line in p.stdout.splitlines():
        line = line.strip()
        if not (line.startswith('[') and ']' in line):
            continue
        # Parse the [N] index.
        try:
            idx_end = line.index(']')
            idx = int(line[1:idx_end].strip())
        except ValueError:
            continue
        rest = line[idx_end + 1:].split()
        # rest = [name, type, addr, off, size, es, flags, link, info, align]
        if len(rest) < 7:
            continue
        out[idx] = {
            'name':  rest[0],
            'type':  rest[1],
            'addr':  int(rest[2], 16) if rest[2] else 0,
            'size':  int(rest[4], 16) if rest[4] else 0,
            'flags': rest[6] if len(rest) > 6 else '',
        }
    return out


def _readelf_symbols(so: str, sections: dict[int, dict]) -> dict[str, dict]:
    """Symbol name → {bind, type, size, section_name, section_type,
    section_flags, is_tls}. Only STT_OBJECT entries are emitted;
    function symbols are filtered. We prefer .dynsym (-D) for visibility
    of exported names — those are what the dynamic linker resolves."""
    out: dict[str, dict] = {}
    # Use both .symtab (-s) and .dynsym (-D, --dyn-syms) so we catch
    # both stripped (.dynsym only) and unstripped (.symtab additionally
    # carries internals). Merge by name; .symtab entries win for type info.
    for flag in ('--dyn-syms', '--syms'):
        try:
            p = subprocess.run(['readelf', '-W', flag, so],
                               capture_output=True, text=True, check=True)
        except subprocess.CalledProcessError:
            continue
        for line in p.stdout.splitlines():
            line = line.strip()
            if not line or 'Num:' in line or 'Symbol table' in line:
                continue
            parts = line.split()
            # readelf format:
            # Num: Value Size Type Bind Vis Ndx Name [Version]
            if len(parts) < 8:
                continue
            try:
                size_str = parts[2]
                size = int(size_str) if size_str.isdigit() else int(size_str, 16)
            except ValueError:
                continue
            stype = parts[3]
            bind = parts[4]
            ndx = parts[6]
            name_and_ver = ' '.join(parts[7:])
            # Strip @VERSION / @@VERSION suffixes.
            name = name_and_ver.split('@', 1)[0]
            if not name or name.startswith('_GLOBAL_OFFSET_TABLE_'):
                continue
            # We want STT_OBJECT (data) and STT_TLS (thread-local data).
            if stype not in ('OBJECT', 'TLS'):
                continue
            # ndx is either a number or 'UND'/'ABS'/'COM'. UND = no
            # definition here; skip.
            if ndx in ('UND', 'ABS', 'COM'):
                continue
            try:
                section_idx = int(ndx)
            except ValueError:
                continue
            sec = sections.get(section_idx)
            if not sec:
                continue
            rec = {
                'bind': bind,
                'stt':  stype,
                'size': size,
                'section_name':  sec['name'],
                'section_type':  sec['type'],
                'section_flags': sec['flags'],
                'is_tls': (stype == 'TLS'
                           or 'T' in sec['flags']
                           or sec['name'].startswith('.tdata')
                           or sec['name'].startswith('.tbss')),
            }
            # First occurrence wins (--dyn-syms first).
            out.setdefault(name, rec)
    return out


# ─── Mach-O readers (darwin / iOS / tvOS libSystem) ───────────────────
#
# Uses llvm-objdump + llvm-nm. Both work on Linux against macOS/iOS
# .dylib files, so the iOS policy can be generated cross-platform.
#
# Section mapping (Mach-O segment,section → our hazard class):
#   __DATA,__data         mutable initialised data    → shared
#   __DATA,__bss          mutable zero-init           → shared
#   __DATA,__const        read-only data              → shared_const
#   __TEXT,__const        read-only (in TEXT segment) → shared_const
#   __DATA,__thread_data  thread-local initialised    → threadlocal
#   __DATA,__thread_bss   thread-local zero-init      → threadlocal
#   __DATA,__common       BSS-equivalent (deprecated) → shared
#
# `llvm-nm --format=posix --defined-only` gives one symbol per line:
#   <name> <kind-char> <vmaddr> <size>
# kind chars (Mach-O subset):
#   S/s = symbol in non-text/data/bss section (we map via objdump)
#   D/d = __DATA,__data
#   B/b = __DATA,__bss / __common
#   R/r = read-only data
#   T/t = code (skipped)
# Capital letter = external (exported), lowercase = local. We keep
# both — local globals can still be SHARED across guests.

def _macho_sections(so: str) -> dict[str, dict]:
    """Section table keyed by '<segment>,<section>' string. Each entry
    gets a normalised hazard class so the symbol pass can attach it."""
    out: dict[str, dict] = {}
    p = subprocess.run(['llvm-objdump', '--macho', '--section-headers', so],
                       capture_output=True, text=True, check=True)
    # Lines look like:
    #   Idx Name             Size     VMA              Type
    #     0 __text           000546aa 0000000000000db0 TEXT
    # We don't need Idx — sections in Mach-O are addressed by
    # (segment, name) and llvm-nm gives us "S" + the full
    # segment,section in its annotated output `nm -m`.
    for line in p.stdout.splitlines():
        line = line.strip()
        if not line or line.startswith(('Sections:', 'Idx ')):
            continue
        parts = line.split()
        if len(parts) < 5 or not parts[0].isdigit():
            continue
        # Name is parts[1]; objdump prints the SECTION name only (no
        # segment). For accurate classification we need the segment too.
        # objdump doesn't print it by default — fall back to llvm-nm
        # below which DOES emit "segment,section" via `-m`.
        out[parts[1]] = {
            'name': parts[1],
            'type': parts[4],   # TEXT / DATA / BSS-like
        }
    return out


def _macho_kind_to_class(secname: str, kind: str) -> tuple[str, bool]:
    """Map a Mach-O '__SEGMENT,__section' string + nm kind-char to
    (hazard_class, is_tls). Mirrors the ELF logic in classify()."""
    sec = secname.lower()
    is_tls = ('__thread' in sec)
    if is_tls:
        return 'threadlocal', True
    # read-only?
    if ('__const' in sec
            or '__cstring' in sec
            or kind in ('R', 'r', 's')):
        return 'shared_const', False
    if ('__data' in sec or '__bss' in sec or '__common' in sec
            or kind in ('D', 'd', 'B', 'b')):
        return 'shared', False
    # Fallback: unknown — treat as shared (worst-case for safety).
    return 'shared', False


def _macho_symbols(so: str) -> dict[str, dict]:
    """Symbol map keyed by name. Uses `llvm-nm -m` which prints
    the FULL '(__SEGMENT,__section) symbol' annotation, giving us
    enough to classify without a separate section-table lookup."""
    out: dict[str, dict] = {}
    # `-m` = display each symbol's section assignment (annotated).
    # `--defined-only` skips undefined imports.
    # `--no-sort` cheap.
    p = subprocess.run(['llvm-nm', '-m', '--defined-only', so],
                       capture_output=True, text=True, check=True)
    # Lines look like:
    #   00000000000004f0 (__DATA,__data) external _foo
    #   0000000000000000 (__TEXT,__text) external _main
    # On universal binaries llvm-nm emits one block per arch with
    # a header line ":arch x86_64" etc. — we don't care which arch
    # (the policy is per ABI but most globals are universal).
    import re
    pat = re.compile(
        r'^\s*([0-9a-fA-F]+)\s+'        # vmaddr
        r'\(([^,]+),([^)]+)\)\s+'        # (segment,section)
        r'(\S+)\s+'                       # binding ("external" / "non-external" / "weak external" / ...)
        r'(\S+)'                          # name
    )
    for line in p.stdout.splitlines():
        m = pat.match(line)
        if not m:
            continue
        vmaddr, seg, sect, binding, name = m.groups()
        # Strip the leading underscore Mach-O adds to C symbols.
        if name.startswith('_'):
            name = name[1:]
        if not name:
            continue
        # We don't care about code symbols (text section).
        if 'text' in sect.lower() or 'stub' in sect.lower():
            continue
        secname = f'{seg},{sect}'
        hazard, is_tls = _macho_kind_to_class(secname, '')
        # Size isn't directly in `-m` output — call llvm-nm again
        # with --print-size if a caller really needs sizes. For now
        # leave 0; the classify() heuristics don't depend on size.
        out.setdefault(name, {
            'bind':         binding,
            'stt':          'TLS' if is_tls else 'OBJECT',
            'size':         0,                # llvm-nm -m omits sizes
            'section_name': secname,
            'section_type': 'TLS' if is_tls else 'PROGBITS',
            'section_flags': 'W' if hazard == 'shared' else '',
            'is_tls':       is_tls,
        })
    # Add sizes via a second pass with --print-size which lists
    # "vmaddr size kind name" but doesn't tell us the section.
    # Cheap to merge by name.
    try:
        p2 = subprocess.run(['llvm-nm', '--format=posix', '--print-size',
                             '--defined-only', so],
                            capture_output=True, text=True, check=True)
        for line in p2.stdout.splitlines():
            parts = line.split()
            if len(parts) < 4:
                continue
            n = parts[0]
            if n.startswith('_'):
                n = n[1:]
            if n in out:
                try:
                    out[n]['size'] = int(parts[3], 16)
                except (ValueError, IndexError):
                    pass
    except subprocess.CalledProcessError:
        pass
    return out


# ─── Hazard classification ────────────────────────────────────────────

# Heuristic: known "mutable in C, semantically immortal" singletons.
# Mostly CPython's deep-immortal objects. We special-case the
# well-known names to avoid the policy generator panicking about
# Py_NoneStruct being "in .data, mutable, no thread-local". It is
# never mutated post-init.
_IMMORTAL_SINGLETONS = {
    '_Py_NoneStruct', '_Py_TrueStruct', '_Py_FalseStruct',
    '_Py_EllipsisObject', '_Py_NotImplementedStruct',
    '_PyByteArray_empty_string',
}


def classify(name: str, decl: dict | None, so: dict) -> str:
    if so['is_tls']:
        return 'threadlocal'
    flags = so['section_flags']
    sec_name = so['section_name']
    # READ-ONLY: 'A' alone (alloc), or 'AR' / 'AR ' — no 'W' write.
    if 'W' not in flags:
        return 'shared_const'
    if name in _IMMORTAL_SINGLETONS:
        return 'immortal_singleton'
    # Anything mutable that survived the above is a cross-guest hazard.
    return 'shared'


# ─── Driver ───────────────────────────────────────────────────────────

def _load_policy(path: str) -> dict:
    """Policy file shape (see policies/libc.yaml for an example):

      library: libc.so.6
      classes:
        bridged_per_ctx:
          - name: environ
            via: impl/env.c        # human breadcrumb
            note: per-ctx env vector replaces host environ
        tls_safe: [errno, __resp, …]
        safe_immutable:
          - {name: sys_errlist, note: error message strings, never written}
        yos_owned:
          - {name: __pthread_keys, note: yos pthread emulation owns it}
        leaks:
          - {name: optind, note: TODO bridge env.getopt with per-ctx state}

    Items can be a bare name string or a {name, …} mapping. Bare-string
    entries get an empty note.
    """
    raw = yaml.safe_load(Path(path).read_text()) or {}
    name_to_class: dict[str, tuple[str, dict]] = {}
    for cls, items in (raw.get('classes') or {}).items():
        for item in (items or []):
            if isinstance(item, str):
                name = item; meta = {'name': name}
            elif isinstance(item, dict):
                name = item.get('name')
                if not name:
                    continue
                meta = item
            else:
                continue
            name_to_class[name] = (cls, meta)
    return {'library': raw.get('library'), 'class_of': name_to_class}


def _coverage(globals_out: list[dict], policy: dict) -> dict:
    """Diff scan against policy. Returns:
      - covered:        list of {name, policy_class, scan_hazard, agrees?}
      - leaks_unpolicied:  scanned-SHARED globals with no policy entry
      - safe_unpolicied:   scanned-non-SHARED with no entry (informational)
      - stale_in_policy:   policy entries naming globals not in scan
    """
    class_of = policy['class_of']
    seen_in_scan = {g['name'] for g in globals_out}

    covered, leaks_unpolicied, safe_unpolicied = [], [], []
    for g in globals_out:
        name = g['name']
        if name in class_of:
            cls, meta = class_of[name]
            # Sanity check: if policy says tls_safe but scan shows shared,
            # something is wrong. Flag for human review.
            agrees = not (cls == 'tls_safe' and g['hazard'] != 'threadlocal')
            covered.append({
                'name':         name,
                'policy_class': cls,
                'scan_hazard':  g['hazard'],
                'agrees':       agrees,
                'note':         meta.get('note', ''),
            })
        else:
            if g['hazard'] == 'shared':
                leaks_unpolicied.append({
                    'name':    name,
                    'size':    g['size'],
                    'section': g['section'],
                })
            else:
                safe_unpolicied.append({
                    'name':    name,
                    'hazard':  g['hazard'],
                })

    stale_in_policy = [n for n in class_of if n not in seen_in_scan]

    by_class: dict[str, int] = {}
    for c in covered:
        by_class[c['policy_class']] = by_class.get(c['policy_class'], 0) + 1

    return {
        'totals': {
            'scanned':           len(globals_out),
            'policy_entries':    len(class_of),
            'covered':           len(covered),
            'leaks_unpolicied':  len(leaks_unpolicied),
            'safe_unpolicied':   len(safe_unpolicied),
            'stale_in_policy':   len(stale_in_policy),
            'by_class':          by_class,
        },
        'covered':            covered,
        'leaks_unpolicied':   leaks_unpolicied,
        'safe_unpolicied':    safe_unpolicied,
        'stale_in_policy':    stale_in_policy,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--so', required=True, help='path to libfoo.so')
    ap.add_argument('--headers', nargs='+', default=[],
                    help='header files to walk (e.g. Python.h)')
    ap.add_argument('--cflags', nargs='*', default=[],
                    help='passed to clang during header walk')
    ap.add_argument('--out', help='write yaml here (default stdout)')
    ap.add_argument('--format', choices=('yaml', 'json'), default='yaml')
    ap.add_argument('--policy', help='diff scan against this policy yaml')
    ap.add_argument('--coverage-out',
                    help='write coverage report YAML here (with --policy)')
    ap.add_argument('--fail-on-leak', action='store_true',
                    help='exit 1 if scanned-SHARED globals are not '
                         'covered by the policy (CI guard)')
    args = ap.parse_args()

    so = os.path.realpath(args.so)
    if not os.path.exists(so):
        sys.exit(f'extract_globals: library not found: {so}')

    fmt = _detect_format(so)
    if fmt == 'elf':
        sections = _readelf_sections(so)
        so_syms  = _readelf_symbols(so, sections)
    elif fmt == 'macho':
        # llvm-nm -m gives us per-symbol section assignment in one pass.
        # We don't need a separate _macho_sections() table feeding into
        # the symbol pass — the section name is embedded per-symbol.
        # _macho_sections is exposed for callers that want the
        # section-level summary independently.
        _ = _macho_sections(so)   # validates llvm-objdump is installed
        so_syms = _macho_symbols(so)
    else:
        sys.exit(f'extract_globals: {so}: unknown binary format '
                 f'(not ELF or Mach-O magic)')

    # Header decls (may be empty if no --headers).
    decls = walk_headers(args.headers, args.cflags) if args.headers else {}

    # Inventory: every STT_OBJECT/TLS in the .so, cross-referenced.
    globals_out = []
    for name in sorted(so_syms):
        so_rec = so_syms[name]
        decl = decls.get(name)
        hazard = classify(name, decl, so_rec)
        item = {
            'name':         name,
            'size':         so_rec['size'],
            'section':      so_rec['section_name'],
            'mutable':      'W' in so_rec['section_flags'],
            'threadlocal':  so_rec['is_tls'],
            'hazard':       hazard,
        }
        if decl:
            item['type']                  = decl['type']
            item['header']                = decl['header']
            item['threadlocal_declared']  = decl['threadlocal_declared']
            item['is_const_declared']     = decl['is_const_declared']
        globals_out.append(item)

    # Decls with NO matching .so symbol — header-only / inlined / dead.
    orphan_decls = [n for n in decls if n not in so_syms]

    summary = {
        'shared':             sum(1 for g in globals_out if g['hazard'] == 'shared'),
        'shared_const':       sum(1 for g in globals_out if g['hazard'] == 'shared_const'),
        'threadlocal':        sum(1 for g in globals_out if g['hazard'] == 'threadlocal'),
        'immortal_singleton': sum(1 for g in globals_out if g['hazard'] == 'immortal_singleton'),
    }
    summary['total'] = sum(summary.values())

    report = {
        'library':       os.path.basename(so),
        'so_path':       so,
        'headers':       args.headers,
        'summary':       summary,
        'orphan_decls':  orphan_decls,
        'globals':       globals_out,
    }

    out_text = (yaml.safe_dump(report, sort_keys=False, default_flow_style=False)
                if args.format == 'yaml' else json.dumps(report, indent=2))

    if args.out:
        Path(args.out).write_text(out_text)
        print(f'extract_globals: wrote {len(globals_out)} entries '
              f'({summary["shared"]} SHARED, {summary["shared_const"]} const, '
              f'{summary["threadlocal"]} TLS, '
              f'{summary["immortal_singleton"]} immortal) → {args.out}',
              file=sys.stderr)
    else:
        sys.stdout.write(out_text)

    # ── Policy diff ────────────────────────────────────────────────────
    if args.policy:
        policy = _load_policy(args.policy)
        cov = _coverage(globals_out, policy)
        t = cov['totals']
        # Always print summary to stderr so CI logs show coverage shift.
        print(
            f'\n=== POLICY COVERAGE ({args.policy}) ===\n'
            f'  scanned globals:      {t["scanned"]}\n'
            f'  policy entries:       {t["policy_entries"]}\n'
            f'  covered by policy:    {t["covered"]}'
            f'    (by class: ' +
            ', '.join(f'{c}={n}' for c, n in sorted(t['by_class'].items()))
            + f')\n'
            f'  UNCOVERED shared (leaks):  {t["leaks_unpolicied"]}\n'
            f'  non-shared not in policy:  {t["safe_unpolicied"]}\n'
            f'  stale policy entries:      {t["stale_in_policy"]}\n',
            file=sys.stderr)
        if cov['leaks_unpolicied']:
            print('--- UNCOVERED SHARED globals ---', file=sys.stderr)
            for g in cov['leaks_unpolicied'][:20]:
                print(f'  {g["size"]:>7}  {g["section"]:<14}  {g["name"]}',
                      file=sys.stderr)
            if len(cov['leaks_unpolicied']) > 20:
                print(f'  ... and {len(cov["leaks_unpolicied"]) - 20} more',
                      file=sys.stderr)
        if cov['stale_in_policy']:
            print(f'--- STALE policy entries (not in .so) ---', file=sys.stderr)
            for n in cov['stale_in_policy'][:20]:
                print(f'  {n}', file=sys.stderr)
            if len(cov['stale_in_policy']) > 20:
                print(f'  ... and {len(cov["stale_in_policy"]) - 20} more',
                      file=sys.stderr)
        if args.coverage_out:
            Path(args.coverage_out).write_text(
                yaml.safe_dump(cov, sort_keys=False, default_flow_style=False))
            print(f'extract_globals: coverage → {args.coverage_out}',
                  file=sys.stderr)
        if args.fail_on_leak and cov['leaks_unpolicied']:
            print(f'\nFAIL: {len(cov["leaks_unpolicied"])} SHARED globals not '
                  f'covered by policy. Either bridge them per-ctx and mark '
                  f'bridged_per_ctx, or explicitly mark them leaks/yos_owned/'
                  f'safe_immutable in {args.policy}.',
                  file=sys.stderr)
            sys.exit(1)


if __name__ == '__main__':
    main()
