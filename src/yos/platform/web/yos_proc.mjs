// Process-aware browser host for asyncified yos wasm guests.
//
// Real fork via asyncify, the same dance native yos runs: fork() calls
// asyncify_start_unwind, the wasm stack pops back to JS, we snapshot the
// linear memory, build a CHILD instance with a copy, and
// asyncify_start_rewind fast-forwards both back to the fork callsite —
// child returns 0, parent returns the child pid. A process table assigns
// pids; waitpid reaps. Cooperative (single-threaded): the child runs to
// completion before the parent resumes, which is correct for
// fork+wait and recursive fork trees (concurrent live children — pipes,
// pthreads — need Workers and come later).
//
// The libc surface is shared with the simple runner via buildLibc().

const ASYNCIFY_NORMAL = 0, ASYNCIFY_UNWINDING = 1, ASYNCIFY_REWINDING = 2;
const ASYNCIFY_BUF_SIZE = 16384;

const enc = new TextEncoder();
const dec = new TextDecoder();
const DEFAULT_ENV = ["PATH=/bin:/usr/bin", "HOME=/", "TERM=xterm-256color", "PWD=/"];

// Per-instance memory state + allocator over the guest's linear memory.
function makeState() {
  const state = { mem: null, view: null, u8: null, brk: 0, main: null, errnoPtr: 0, nextFd: 10, asyncifyPtr: 0 };
  state.refresh = () => { state.view = new DataView(state.mem.buffer); state.u8 = new Uint8Array(state.mem.buffer); };
  // The whole process tree runs in one synchronous JS turn (no GC
  // between forks), so every live instance's memory adds up. Modest
  // increments balance per-instance size against grow-call overhead; a
  // very deep recursive tree (100+) still pushes the browser's wasm
  // budget, so use a smaller fanout in-browser.
  state.grow = (need) => { while (state.brk + need > state.u8.length) { const before = state.u8.length; state.mem.grow(64); state.refresh(); if (state.u8.length === before) throw new Error("out of wasm memory"); } };
  state.alloc = (n, align = 8) => { state.brk = (state.brk + (align - 1)) & ~(align - 1); state.grow(n); const p = state.brk; state.brk += n; return p; };
  state.putStr = (s) => { const b = enc.encode(s + "\0"); const p = state.alloc(b.length, 1); state.u8.set(b, p); return p; };
  state.cstr = (ptr) => { if (!ptr) return ""; let e = ptr; while (state.u8[e]) e++; return dec.decode(state.u8.subarray(ptr, e)); };
  return state;
}

function formatFromGuest(state, fmtPtr, vaPtr) {
  const fmt = state.cstr(fmtPtr);
  let va = vaPtr;
  // wasm32/i386: int/long/size_t/ptr = 4 bytes; long long/intmax = 8;
  // double = 8. The length modifier decides 4-vs-8 for integers.
  const n4 = () => { va = (va + 3) & ~3; const v = state.view.getInt32(va, true); va += 4; return v; };
  const u4 = () => { va = (va + 3) & ~3; const v = state.view.getUint32(va, true); va += 4; return v; };
  const n8 = () => { va = (va + 7) & ~7; const lo = state.view.getUint32(va, true), hi = state.view.getInt32(va + 4, true); va += 8; return hi * 4294967296 + lo; };
  const u8int = () => { va = (va + 7) & ~7; const lo = state.view.getUint32(va, true), hi = state.view.getUint32(va + 4, true); va += 8; return hi * 4294967296 + lo; };
  const f8 = () => { va = (va + 7) & ~7; const v = state.view.getFloat64(va, true); va += 8; return v; };
  let out = "";
  for (let i = 0; i < fmt.length; i++) {
    if (fmt[i] !== "%") { out += fmt[i]; continue; }
    let spec = "%"; i++;
    while (i < fmt.length && "-+ #0".includes(fmt[i])) spec += fmt[i++];
    let width = ""; while (i < fmt.length && /[0-9]/.test(fmt[i])) { width += fmt[i]; spec += fmt[i++]; }
    let prec = ""; if (fmt[i] === ".") { spec += fmt[i++]; while (i < fmt.length && /[0-9]/.test(fmt[i])) { prec += fmt[i]; spec += fmt[i++]; } }
    let lengthMod = ""; while (i < fmt.length && "hljztLq".includes(fmt[i])) { lengthMod += fmt[i]; spec += fmt[i++]; }
    const wide = /ll|j|q/.test(lengthMod); // 8-byte integer
    const conv = fmt[i];
    const w = width ? parseInt(width, 10) : 0;
    const pad = (s) => (w && s.length < w ? (spec.includes("-") ? s.padEnd(w) : s.padStart(w)) : s);
    switch (conv) {
      case "d": case "i": out += pad(String(wide ? n8() : n4())); break;
      case "u": out += pad(String(wide ? u8int() : u4())); break;
      case "x": out += pad((wide ? u8int() : u4()).toString(16)); break;
      case "X": out += pad((wide ? u8int() : u4()).toString(16).toUpperCase()); break;
      case "o": out += pad((wide ? u8int() : u4()).toString(8)); break;
      case "p": out += "0x" + u4().toString(16); break;
      case "c": out += String.fromCharCode(n4() & 0xff); break;
      case "s": { const sp = u4(); let s = state.cstr(sp); if (prec) s = s.slice(0, parseInt(prec, 10)); out += pad(s); break; }
      case "f": case "F": case "g": case "G": case "e": out += pad(String(f8())); break;
      case "%": out += "%"; break;
      default: out += spec + (conv || ""); break;
    }
  }
  return out;
}

// Fill one FreeBSD-i386 kinfo_proc record (768 bytes) at `off`, with the
// exact offsets the real ps reads.
function fillKinfoProc(state, off, p) {
  const v = state.view, u = state.u8;
  u.fill(0, off, off + 768);
  v.setInt32(off + 0, 768, true);       // ki_structsize
  v.setInt32(off + 40, p.pid, true);    // ki_pid
  v.setInt32(off + 44, p.ppid, true);   // ki_ppid
  v.setInt32(off + 48, p.pgid || p.pid, true); // ki_pgid
  v.setInt32(off + 56, p.sid || p.pid, true);  // ki_sid
  v.setInt32(off + 516, 1, true);       // ki_numthreads
  v.setInt32(off + 520, p.pid, true);   // ki_tid
  v.setUint32(off + 136, 0, true);      // ki_uid
  v.setUint32(off + 140, 0, true);      // ki_ruid
  u[off + 308] = p.exited ? 5 : 2;      // ki_stat: SZOMB(5) / SRUN(2)
  const comm = enc.encode((p.comm || "?").slice(0, 19));
  u.set(comm, off + 367);               // ki_comm[20]
  u.set(comm.subarray(0, 16), off + 314); // ki_tdname[17]
}

// FreeBSD-i386 struct stat: mode@24(2B), uid@28, gid@32, mtime@64,
// size@96(8B), blksize@112.
function fillStat(state, off, node) {
  const v = state.view, u = state.u8;
  u.fill(0, off, off + 128);
  const mode = node.type === "dir" ? S_IFDIR | 0o755 : node.type === "char" ? S_IFCHR | 0o666 : S_IFREG | 0o644;
  v.setUint32(off + 8, node.ino || 1, true);                 // st_ino
  v.setUint32(off + 16, node.type === "dir" ? 2 : 1, true);  // st_nlink
  v.setUint16(off + 24, mode, true);                          // st_mode
  v.setUint32(off + 52, node.mtime || 0, true);              // st_atim.sec
  v.setUint32(off + 64, node.mtime || 0, true);              // st_mtim.sec
  v.setUint32(off + 76, node.mtime || 0, true);              // st_ctim.sec
  const size = node.type === "file" ? node.data.length : (node.entries ? node.entries.length * 64 : 0);
  v.setUint32(off + 96, size, true);                          // st_size
  v.setUint32(off + 112, 4096, true);                         // st_blksize
}

// FreeBSD dirent: d_fileno@0(8B), d_reclen@16(2B), d_type@18, d_namlen@20, d_name@24.
function fillDirent(state, off, name, isDir, ino) {
  const v = state.view, u = state.u8;
  u.fill(0, off, off + 280);
  v.setUint32(off + 0, ino || 1, true);          // d_fileno
  v.setUint16(off + 16, 280, true);              // d_reclen
  u[off + 18] = isDir ? 4 : 8;                   // d_type DT_DIR/DT_REG
  const nb = new TextEncoder().encode(name);
  v.setUint16(off + 20, nb.length, true);        // d_namlen
  u.set(nb, off + 24);                           // d_name
}

// Build the libc env for one process. `proc` and `mgr` give fork/exec/
// wait access to the process table; `io` is { onOutput, onUnimpl }.
function buildLibc(state, env_vars, io, mgr, proc) {
  const u8 = () => state.u8, view = () => state.view;
  const cstr = (p) => state.cstr(p), putStr = (s) => state.putStr(s);
  const basename = (p) => { const i = p.lastIndexOf("/"); return i < 0 ? p : p.slice(i + 1); };
  const writeBytes = (fd, ptr, len) => {
    const f = mgr.openFiles.get(fd);
    if (f && f.node && f.node.type === "file") { // write into the VFS file
      const at = f.append ? f.node.data.length : f.off;
      const merged = new Uint8Array(Math.max(f.node.data.length, at + len));
      merged.set(f.node.data); merged.set(state.u8.subarray(ptr, ptr + len), at);
      f.node.data = merged; f.off = at + len; return len;
    }
    io.onOutput(fd, dec.decode(state.u8.subarray(ptr, ptr + len))); return len; // stdio → terminal
  };
  const fdOfFile = (fp) => (fp >= 1 && fp <= 3 ? fp - 1 : 1);
  const emit = (fd, text) => io.onOutput(fd, text);
  const fmt = (f, v) => formatFromGuest(state, f, v);
  const exitWith = (code) => { const e = new Error("exit"); e.isExit = true; e.code = code | 0; throw e; };

  const env = {};
  for (const { name } of WebAssembly.Module.imports(proc.mod).filter((i) => i.kind === "function")) {
    env[name] = (() => { let seen = false; return (...a) => { if (!seen) { seen = true; io.onUnimpl && io.onUnimpl(name, a); } return 0; }; })();
  }

  Object.assign(env, {
    __main_argc_argv: (argc, argv2) => state.main(argc, argv2),
    __yos_argc: () => proc.argv.length,
    __yos_argv_setup: (p) => { for (let i = 0; i < proc.argv.length; i++) view().setUint32(p + i * 4, putStr(proc.argv[i]), true); },
    __yos_envc: () => env_vars.length,
    __yos_envp_setup: (p) => { for (let i = 0; i < env_vars.length; i++) view().setUint32(p + i * 4, putStr(env_vars[i]), true); },
    exit: exitWith, _exit: exitWith,
    abort: () => exitWith(134),

    malloc: (n) => state.alloc(n),
    calloc: (a, b) => { const p = state.alloc(a * b); u8().fill(0, p, p + a * b); return p; },
    realloc: (p, n) => { const q = state.alloc(n); if (p) u8().copyWithin(q, p, p + n); return q; },
    free: () => {},
    memset: (d, c, n) => { u8().fill(c & 0xff, d, d + n); return d; },
    bzero: (d, n) => { u8().fill(0, d, d + n); return 0; },
    memcpy: (d, s, n) => { u8().copyWithin(d, s, s + n); return d; },
    memmove: (d, s, n) => { u8().copyWithin(d, s, s + n); return d; },

    __error: () => state.errnoPtr,
    getenv: (n) => { const k = cstr(n); const h = env_vars.find((e) => e.startsWith(k + "=")); return h ? putStr(h.slice(k.length + 1)) : 0; },
    setenv: () => 0, unsetenv: () => 0, putenv: () => 0,
    setlocale: () => putStr("C"), nl_langinfo: () => putStr(""), ___mb_cur_max: () => 1,
    sysconf: () => -1,
    getuid: () => 0, geteuid: () => 0, getgid: () => 0, getegid: () => 0,
    getpid: () => proc.pid, getppid: () => proc.ppid, getpgrp: () => proc.pid, getgroups: () => 0,
    isatty: () => 0,

    write: (fd, p, l) => writeBytes(fd, p, l),
    writev: (fd, iov, c) => { let t = 0; for (let i = 0; i < c; i++) { const b = view().getUint32(iov + i * 8, true); const l = view().getUint32(iov + i * 8 + 4, true); t += writeBytes(fd, b, l); } return t; },
    printf: (f, v) => { const s = fmt(f, v); emit(1, s); return s.length; },
    vprintf: (f, v) => { const s = fmt(f, v); emit(1, s); return s.length; },
    fprintf: (fp, f, v) => { const s = fmt(f, v); emit(fdOfFile(fp), s); return s.length; },
    vfprintf: (fp, f, v) => { const s = fmt(f, v); emit(fdOfFile(fp), s); return s.length; },
    snprintf: (d, n, f, v) => { const s = fmt(f, v); const b = enc.encode(s).subarray(0, Math.max(0, n - 1)); u8().set(b, d); u8()[d + b.length] = 0; return s.length; },
    vsnprintf: (d, n, f, v) => env.snprintf(d, n, f, v),
    sprintf: (d, f, v) => { const s = fmt(f, v); const b = enc.encode(s); u8().set(b, d); u8()[d + b.length] = 0; return s.length; },
    vsprintf: (d, f, v) => env.sprintf(d, f, v),
    fputc: (c, fp) => { emit(fdOfFile(fp), String.fromCharCode(c & 0xff)); return c & 0xff; },
    putc: (c, fp) => env.fputc(c, fp), putchar: (c) => { emit(1, String.fromCharCode(c & 0xff)); return c & 0xff; },
    fputs: (s, fp) => { emit(fdOfFile(fp), cstr(s)); return 1; },
    puts: (s) => { emit(1, cstr(s) + "\n"); return 1; },
    fwrite: (p, sz, nm, fp) => { writeBytes(fdOfFile(fp), p, sz * nm); return nm; },
    fflush: () => 0, fileno: (fp) => fp, setvbuf: () => 0, setbuf: () => 0,
    // stdio putc-overflow handler: the guest's streams are unbuffered, so
    // every putc/putchar char arrives here. Emit it (NOT discard).
    __swbuf: (c, fp) => { emit(fdOfFile(fp), String.fromCharCode(c & 0xff)); return c & 0xff; },
    clearerr: () => 0, ferror: () => 0, feof: () => 0,

    strlen: (p) => { let n = 0; while (u8()[p + n]) n++; return n; },
    strcmp: (a, b) => { let i = 0; for (;;) { const x = u8()[a + i], y = u8()[b + i]; if (x !== y) return x - y; if (!x) return 0; i++; } },
    strncmp: (a, b, n) => { for (let i = 0; i < n; i++) { const x = u8()[a + i], y = u8()[b + i]; if (x !== y) return x - y; if (!x) return 0; } return 0; },
    strcasecmp: (a, b) => { const lc = (c) => (c >= 65 && c <= 90 ? c + 32 : c); let i = 0; for (;;) { const x = lc(u8()[a + i]), y = lc(u8()[b + i]); if (x !== y) return x - y; if (!x) return 0; i++; } },
    strncasecmp: (a, b, n) => { const lc = (c) => (c >= 65 && c <= 90 ? c + 32 : c); for (let i = 0; i < n; i++) { const x = lc(u8()[a + i]), y = lc(u8()[b + i]); if (x !== y) return x - y; if (!x) return 0; } return 0; },
    strcoll: (a, b) => env.strcmp(a, b),
    strchr: (s, c) => { c &= 0xff; for (let p = s; ; p++) { if (u8()[p] === c) return p; if (!u8()[p]) return c === 0 ? p : 0; } },
    strrchr: (s, c) => { c &= 0xff; let hit = 0; for (let p = s; ; p++) { if (u8()[p] === c) hit = p; if (!u8()[p]) return c === 0 ? p : hit; } },
    strstr: (h, n) => { const needle = cstr(n); if (!needle) return h; const hay = cstr(h); const idx = hay.indexOf(needle); return idx < 0 ? 0 : h + enc.encode(hay.slice(0, idx)).length; },
    strcpy: (d, s) => { let i = 0; do { u8()[d + i] = u8()[s + i]; } while (u8()[s + i++]); return d; },
    strncpy: (d, s, n) => { let i = 0; for (; i < n && u8()[s + i]; i++) u8()[d + i] = u8()[s + i]; for (; i < n; i++) u8()[d + i] = 0; return d; },
    strlcpy: (d, s, n) => { const len = env.strlen(s); if (n) { const c = Math.min(len, n - 1); u8().copyWithin(d, s, s + c); u8()[d + c] = 0; } return len; },
    strcat: (d, s) => { env.strcpy(d + env.strlen(d), s); return d; },
    strncat: (d, s, n) => { let dl = env.strlen(d), i = 0; for (; i < n && u8()[s + i]; i++) u8()[d + dl + i] = u8()[s + i]; u8()[d + dl + i] = 0; return d; },
    strdup: (s) => putStr(cstr(s)),
    strspn: (s, set) => { const ss = cstr(set); let n = 0; for (;;) { const c = u8()[s + n]; if (!c || !ss.includes(String.fromCharCode(c))) return n; n++; } },
    strcspn: (s, set) => { const ss = cstr(set); let n = 0; for (;;) { const c = u8()[s + n]; if (!c || ss.includes(String.fromCharCode(c))) return n; n++; } },
    memchr: (s, c, n) => { c &= 0xff; for (let i = 0; i < n; i++) if (u8()[s + i] === c) return s + i; return 0; },
    memcmp: (a, b, n) => { for (let i = 0; i < n; i++) { const x = u8()[a + i], y = u8()[b + i]; if (x !== y) return x - y; } return 0; },
    strtoul: (s, endp, base) => { const str = cstr(s); const m = str.match(/^\s*[+-]?(0x[0-9a-fA-F]+|[0-9]+)/); const v = m ? parseInt(m[0], base || (m[0].includes("0x") ? 16 : 10)) >>> 0 : 0; if (endp) view().setUint32(endp, s + (m ? m[0].length : 0), true); return v; },
    strtol: (s, e, b) => env.strtoul(s, e, b) | 0,
    strtod: (s) => { const v = parseFloat(cstr(s)); return isNaN(v) ? 0 : v; },
    strtonum: (s) => { const v = parseInt(cstr(s), 10); return isNaN(v) ? 0 : v; },
    atoi: (s) => parseInt(cstr(s), 10) || 0,
    strerror: (n) => putStr("Error " + n),
    qsort: () => 0,
    strcasestr: (h, n) => { const needle = cstr(n).toLowerCase(); if (!needle) return h; const hay = cstr(h); const idx = hay.toLowerCase().indexOf(needle); return idx < 0 ? 0 : h + enc.encode(hay.slice(0, idx)).length; },
    strsep: (stringpPtr, delimPtr) => {
      const start = view().getUint32(stringpPtr, true);
      if (!start) return 0;
      const delim = cstr(delimPtr);
      for (let p = start; u8()[p]; p++) {
        if (delim.includes(String.fromCharCode(u8()[p]))) {
          u8()[p] = 0;
          view().setUint32(stringpPtr, p + 1, true);
          return start;
        }
      }
      view().setUint32(stringpPtr, 0, true);
      return start;
    },
    vasprintf: (strpPtr, f, v) => { const s = fmt(f, v); const p = putStr(s); view().setUint32(strpPtr, p, true); return enc.encode(s).length; },
    asprintf: (strpPtr, f, v) => env.vasprintf(strpPtr, f, v),
    tzset: () => 0,
    strndup: (s, n) => { let len = 0; while (len < n && u8()[s + len]) len++; const p = state.alloc(len + 1); u8().copyWithin(p, s, s + len); u8()[p + len] = 0; return p; },
    strpbrk: (s, set) => { const ss = cstr(set); for (let p = s; u8()[p]; p++) if (ss.includes(String.fromCharCode(u8()[p]))) return p; return 0; },
    memmem: (h, hn, n, nn) => { if (!nn) return h; for (let i = 0; i + nn <= hn; i++) { let m = true; for (let j = 0; j < nn; j++) if (u8()[h + i + j] !== u8()[n + j]) { m = false; break; } if (m) return h + i; } return 0; },
    mempcpy: (d, s, n) => { u8().copyWithin(d, s, s + n); return d + n; },

    // --- process syscalls ---
    fork: () => mgr.fork(proc),
    vfork: () => mgr.fork(proc),
    waitpid: (pid, statusPtr, opts) => mgr.waitpid(proc, pid, statusPtr),
    wait3: (statusPtr) => mgr.waitpid(proc, -1, statusPtr),
    wait4: (pid, statusPtr) => mgr.waitpid(proc, pid, statusPtr),
    access: (pathPtr) => (mgr.tools.has(basename(cstr(pathPtr))) ? 0 : -1),
    execve: (pathPtr, argvPtr) => {
      const name = basename(cstr(pathPtr));
      if (!mgr.tools.has(name)) { if (state.errnoPtr) view().setUint32(state.errnoPtr, 2, true); return -1; }
      const args = [];
      for (let p = argvPtr; ; p += 4) { const sp = view().getUint32(p, true); if (!sp) break; args.push(cstr(sp)); }
      const res = mgr.runChildProgram(mgr.tools.get(name), args.length ? args : [name], proc.pid);
      exitWith(res.exitCode);
    },

    // --- VFS: real in-memory filesystem behind open/read/stat/readdir ---
    // FreeBSD open flags: O_APPEND=0x8, O_CREAT=0x200, O_TRUNC=0x400.
    open: (pathPtr, flags) => {
      const path = mgr.vfsPath(cstr(pathPtr));
      let node = mgr.vfs[path];
      if (!node) {
        if (flags & 0x200) node = mgr.vfsCreateFile(path);
        else { if (state.errnoPtr) view().setUint32(state.errnoPtr, 2, true); return -1; }
      }
      if (node.type === "file" && (flags & 0x400)) node.data = new Uint8Array(0); // O_TRUNC
      const fd = mgr.nextFdNum++;
      mgr.openFiles.set(fd, { node, off: (flags & 0x8) && node.data ? node.data.length : 0, path, append: !!(flags & 0x8) });
      return fd;
    },
    openat: (dfd, pathPtr, flags) => env.open(pathPtr, flags),
    close: (fd) => { mgr.openFiles.delete(fd); mgr.openDirs.delete(fd); return 0; },
    read: (fd, buf, n) => {
      const f = mgr.openFiles.get(fd); if (!f || f.node.type !== "file") return 0;
      const data = f.node.data, end = Math.min(f.off + n, data.length);
      u8().set(data.subarray(f.off, end), buf); const got = end - f.off; f.off = end; return got;
    },
    lseek: (fd, off, whence) => { const f = mgr.openFiles.get(fd); if (!f) return -1; f.off = whence === 2 ? f.node.data.length + off : whence === 1 ? f.off + off : off; return f.off; },
    fcntl: (fd, cmd) => (cmd === 0 ? state.nextFd++ : 0),
    dup: () => state.nextFd++, dup2: (a, b) => b,
    pipe: (p) => { view().setUint32(p, state.nextFd++, true); view().setUint32(p + 4, state.nextFd++, true); return 0; },
    ttyname: () => 0, tcgetattr: () => -1, tcsetattr: () => 0, tcgetpgrp: () => -1, tcsetpgrp: () => 0,
    ioctl: () => -1, poll: () => 0, select: () => 0,
    stat: (pathPtr, statBuf) => { const node = mgr.vfsResolve(cstr(pathPtr)); if (!node) { if (state.errnoPtr) view().setUint32(state.errnoPtr, 2, true); return -1; } fillStat(state, statBuf, node); return 0; },
    lstat: (pathPtr, statBuf) => env.stat(pathPtr, statBuf),
    fstatat: (dfd, pathPtr, statBuf) => {
      const name = cstr(pathPtr);
      const base = mgr.openDirs.get(dfd) || mgr.openFiles.get(dfd);
      const baseDir = base ? base.path : mgr.cwd;
      const full = name.startsWith("/") ? name : (baseDir === "/" ? "" : baseDir) + "/" + name;
      const node = mgr.vfs[mgr.vfsPath(full)];
      if (!node) { if (state.errnoPtr) view().setUint32(state.errnoPtr, 2, true); return -1; }
      fillStat(state, statBuf, node); return 0;
    },
    fstat: (fd, statBuf) => {
      if (fd >= 0 && fd <= 2) { fillStat(state, statBuf, { type: "char" }); return 0; } // stdio = char device
      const f = mgr.openFiles.get(fd) || mgr.openDirs.get(fd); if (!f) return -1; fillStat(state, statBuf, f.node); return 0;
    },
    fstatfs: (fd, buf) => { u8().fill(0, buf, buf + 256); return 0; },
    statfs: (p, buf) => { u8().fill(0, buf, buf + 256); return 0; },
    opendir: (pathPtr) => {
      const path = mgr.vfsPath(cstr(pathPtr)); const node = mgr.vfs[path];
      if (!node || node.type !== "dir") { if (state.errnoPtr) view().setUint32(state.errnoPtr, 2, true); return 0; }
      const handle = mgr.nextFdNum++;
      const direntBuf = state.alloc(280, 8);
      mgr.openDirs.set(handle, { node, names: [".", "..", ...node.entries], idx: 0, direntBuf, path });
      return handle;
    },
    fdopendir: (fd) => {
      const f = mgr.openFiles.get(fd);
      if (f && f.node && f.node.type === "dir") mgr.openDirs.set(fd, { node: f.node, names: [".", "..", ...f.node.entries], idx: 0, direntBuf: state.alloc(280, 8), path: f.path });
      return fd;
    },
    readdir: (handle) => {
      const d = mgr.openDirs.get(handle);
      if (!d) return 0;
      if (d.idx >= d.names.length) return 0;
      const name = d.names[d.idx++];
      const child = name === "." || name === ".." ? d.node : mgr.vfs[(d.path === "/" ? "" : d.path) + "/" + name];
      fillDirent(state, d.direntBuf, name, child && child.type === "dir", child && child.ino);
      return d.direntBuf;
    },
    closedir: (handle) => { mgr.openDirs.delete(handle); return 0; },
    dirfd: (handle) => handle,
    umask: () => 0o22, getcwd: (buf, n) => { u8().set(enc.encode(mgr.cwd + "\0"), buf); return buf; },
    chdir: (pathPtr) => { const p = mgr.vfsPath(cstr(pathPtr)); if (mgr.vfs[p] && mgr.vfs[p].type === "dir") { mgr.cwd = p; return 0; } return -1; },
    // fts descends via fchdir(dirfd) then lstats entries by relative name,
    // so fchdir MUST move the cwd or every entry's lstat misses.
    fchdir: (fd) => { const d = mgr.openDirs.get(fd) || mgr.openFiles.get(fd); if (d && d.path && (!d.node || d.node.type === "dir")) mgr.cwd = d.path; return 0; },
    readlink: () => -1, realpath: (pathPtr, out) => { const p = mgr.vfsPath(cstr(pathPtr)); u8().set(enc.encode(p + "\0"), out); return out; },
    signal: () => 0, sigaction: () => 0, sigemptyset: () => 0, sigfillset: () => 0,
    sigaddset: () => 0, sigprocmask: () => 0, sigsuspend: () => 0,
    alarm: () => 0, kill: (pid, sig) => mgr.kill(pid, sig), killpg: () => 0, setpgid: () => 0, getlogin: () => 0,
    getrlimit: () => 0, setrlimit: () => 0, getrusage: () => 0,
    gethostname: (buf, len) => { u8().set(enc.encode("yos-web".slice(0, len - 1) + "\0"), buf); return 0; },
    time: (t) => { const s = Math.floor(Date.now() / 1000); if (t) view().setUint32(t, s, true); return s; },
    gettimeofday: (tv) => { const ms = Date.now(); if (tv) { view().setUint32(tv, Math.floor(ms / 1000), true); view().setUint32(tv + 4, (ms % 1000) * 1000, true); } return 0; },
    clock_gettime: (id, ts) => { const ms = Date.now(); if (ts) { view().setUint32(ts, Math.floor(ms / 1000), true); view().setUint32(ts + 4, (ms % 1000) * 1e6, true); } return 0; },
    nanosleep: () => 0, sleep: () => 0, srand: () => 0, rand: () => 42,
    // No threads yet (cooperative single-thread engine). Fail thread
    // creation fast with EAGAIN so thread-dependent phases skip instead
    // of deadlocking on a condvar waiting for a thread that never runs.
    pthread_create: () => 11, pthread_join: () => 0, pthread_detach: () => 0,
    pthread_mutex_init: () => 0, pthread_mutex_lock: () => 0, pthread_mutex_unlock: () => 0, pthread_mutex_destroy: () => 0,
    pthread_cond_init: () => 0, pthread_cond_signal: () => 0, pthread_cond_broadcast: () => 0, pthread_cond_destroy: () => 0,
    // With no real threads, blocking on a condvar can never be woken — it
    // is a guaranteed deadlock. Throw so the run aborts cleanly instead of
    // busy-looping forever and freezing the page. (Real threads make this
    // a true wait; that is the Worker-pool integration.)
    pthread_cond_wait: () => { const e = new Error("pthread_cond_wait: deadlock (no threads in this engine)"); e.isExit = true; e.code = 75; throw e; },
    pthread_cond_timedwait: () => -1,
    pthread_rwlock_init: () => 0, pthread_rwlock_rdlock: () => 0, pthread_rwlock_wrlock: () => 0, pthread_rwlock_unlock: () => 0, pthread_rwlock_destroy: () => 0,

    // sysctl(CTL_KERN, KERN_PROC, ...) → stream the process table as
    // FreeBSD-i386 kinfo_proc records (768 bytes each) so ps sees real
    // processes. Two-call protocol: size query (oldp=0) then fill.
    sysctl: (namePtr, namelen, oldp, oldlenp) => {
      const mib = [];
      for (let i = 0; i < namelen; i++) mib.push(view().getInt32(namePtr + i * 4, true));
      if (mib[0] === 1 && mib[1] === 14) { // CTL_KERN.KERN_PROC
        const selector = mib[2], pidArg = mib[3]; // PID=1, ALL=0, PROC=8
        const live = mgr.procs.filter((p) => !p.reaped && (selector !== 1 || p.pid === pidArg));
        const need = live.length * 768;
        const buflen = oldlenp ? view().getUint32(oldlenp, true) : 0;
        if (!oldp || buflen < need) {
          if (oldlenp) view().setUint32(oldlenp, need, true);
          if (!oldp) return 0;
          if (state.errnoPtr) view().setUint32(state.errnoPtr, 12, true); // ENOMEM
          return -1;
        }
        let out = oldp;
        for (const p of live) { fillKinfoProc(state, out, p); out += 768; }
        if (oldlenp) view().setUint32(oldlenp, need, true);
        return 0;
      }
      if (oldlenp) view().setUint32(oldlenp, 0, true);
      return 0;
    },
    __xuname: () => 0,
    getpwuid: () => 0, getpwnam: () => 0, getgrgid: () => 0, getgrnam: () => 0,
    user_from_uid: (uid) => putStr(uid === 0 ? "root" : String(uid)),
    group_from_gid: (gid) => putStr(gid === 0 ? "wheel" : String(gid)),
    // multibyte: treat input as single-byte (C/ASCII locale).
    mbrtowc: (pwc, s, n, ps) => { if (!s || n === 0) return 0; const c = u8()[s]; if (pwc) view().setUint32(pwc, c, true); return c === 0 ? 0 : 1; },
    mbtowc: (pwc, s, n) => { if (!s) return 0; const c = u8()[s]; if (pwc) view().setUint32(pwc, c, true); return c === 0 ? 0 : 1; },
    wcwidth: () => 1, wcrtomb: (s, wc) => { if (s) u8()[s] = wc & 0xff; return 1; },
    getbsize: (hdr, lenp) => { if (lenp) view().setUint32(lenp, 512, true); return putStr("512"); },
    humanize_number: () => -1, strmode: (mode, buf) => { u8().set(enc.encode("-rw-r--r-- \0"), buf); return 0; },
  });
  return env;
}

// A tiny in-memory filesystem so ls/cat see real files. dirs hold child
// names; files hold byte data.
function buildVfs(toolNames) {
  const now = Math.floor(Date.now() / 1000);
  let ino = 2;
  const dir = (entries) => ({ type: "dir", entries, mtime: now, ino: ino++ });
  const file = (text) => ({ type: "file", data: new TextEncoder().encode(text), mtime: now, ino: ino++ });
  return {
    "/": dir(["bin", "etc", "home", "README"]),
    "/bin": dir(toolNames.slice().sort()),
    "/etc": dir(["motd", "hostname"]),
    "/etc/motd": file("Welcome to yos in the browser.\n"),
    "/etc/hostname": file("yos-web\n"),
    "/README": file("This is a real file on a real (in-memory) filesystem,\nread by the real cat/ls wasm through env.open/read/readdir.\n"),
    "/home": dir(["user"]),
    "/home/user": dir([".profile"]),
    "/home/user/.profile": file("export PATH=/bin\n"),
    "/tmp": dir([]),
  };
}

const S_IFDIR = 0o040000, S_IFREG = 0o100000, S_IFCHR = 0o020000;

class Manager {
  constructor(io, tools) {
    this.io = io; this.tools = tools || new Map();
    this.nextPid = 1; this.procs = []; this.depth = 0;
    this.vfs = buildVfs([...this.tools.keys()]);
    this.openFiles = new Map(); // fd -> { path, off }
    this.openDirs = new Map();  // handle -> { path, names, idx }
    this.nextFdNum = 5;
    this.nextIno = 100;
    this.cwd = "/";
  }
  vfsPath(path) {
    if (!path) return this.cwd;
    if (path[0] !== "/") path = (this.cwd === "/" ? "" : this.cwd) + "/" + path;
    const stack = [];
    for (const part of path.split("/")) {
      if (!part || part === ".") continue;
      else if (part === "..") stack.pop();
      else stack.push(part);
    }
    return "/" + stack.join("/");
  }
  vfsResolve(path) { return this.vfs[this.vfsPath(path)] || null; }
  vfsCreateFile(path) {
    const node = { type: "file", data: new Uint8Array(0), mtime: Math.floor(Date.now() / 1000), ino: this.nextIno++ };
    this.vfs[path] = node;
    const slash = path.lastIndexOf("/");
    const parent = this.vfs[slash === 0 ? "/" : path.slice(0, slash)];
    const name = path.slice(slash + 1);
    if (parent && parent.type === "dir" && !parent.entries.includes(name)) parent.entries.push(name);
    return node;
  }

  spawn(mod, argv, ppid, env_vars) {
    const pid = this.nextPid++;
    const comm = (argv[0] || "?").replace(/^.*\//, "").slice(0, 19);
    const proc = { pid, ppid, pgid: pid, sid: pid, comm, mod, argv, exited: false, reaped: false, exitCode: 0, signal: 0, forkReturn: 0, forkPending: false, pendingRewind: false, asyncifyPtr: 0, state: null, inst: null };
    const state = makeState();
    proc.state = state;
    const env = buildLibc(state, env_vars || DEFAULT_ENV, this.io, this, proc);
    proc.inst = new WebAssembly.Instance(mod, { env });
    state.mem = proc.inst.exports.memory;
    state.main = proc.inst.exports.main;
    state.refresh();
    const hb = proc.inst.exports.__heap_base;
    state.brk = (typeof hb === "object" ? hb.value : hb) ?? (1 << 20);
    state.errnoPtr = state.alloc(4);
    this.procs.push(proc);
    return proc;
  }

  asyncifyState(proc) { const f = proc.inst.exports.asyncify_get_state; return f ? f() : -1; }

  // Drive one process's _start with the asyncify fork pump.
  run(proc) {
    if (++this.depth > 4096) { this.depth--; throw new Error("fork depth exceeded"); }
    try {
      for (;;) {
        try {
          if (proc.pendingRewind) { proc.pendingRewind = false; proc.inst.exports.asyncify_start_rewind(proc.asyncifyPtr); }
          proc.inst.exports._start();
        } catch (e) {
          if (e.isExit) { proc.exited = true; proc.exitCode = e.code; return; }
          proc.error = e.message; proc.exited = true; proc.exitCode = 139; proc.signal = 11; return;
        }
        if (proc.forkPending) { this.doFork(proc); continue; } // re-enter parent via rewind
        proc.exited = true; return; // _start returned without exit/fork
      }
    } finally { this.depth--; }
  }

  // env.fork: NORMAL → trigger unwind; REWINDING → return the stored value.
  fork(proc) {
    const st = this.asyncifyState(proc);
    if (st < 0) return -1; // not asyncified — fork unsupported
    if (st === ASYNCIFY_REWINDING) { proc.inst.exports.asyncify_stop_rewind(); return proc.forkReturn; }
    if (proc.asyncifyPtr === 0) proc.asyncifyPtr = proc.state.alloc(ASYNCIFY_BUF_SIZE, 8);
    const buf = proc.asyncifyPtr;
    proc.state.view.setUint32(buf, buf + 8, true);
    proc.state.view.setUint32(buf + 4, buf + ASYNCIFY_BUF_SIZE, true);
    proc.forkPending = true;
    proc.inst.exports.asyncify_start_unwind(buf);
    return 0; // ignored — unwinding
  }

  doFork(parent) {
    parent.forkPending = false;
    const child = this.spawn(parent.mod, parent.argv, parent.pid, undefined);
    // Snapshot parent linear memory into the child (grow to match, copy).
    const src = parent.state.u8;
    while (child.state.u8.length < src.length) { const before = child.state.u8.length; child.state.mem.grow(64); child.state.refresh(); if (child.state.u8.length === before) throw new Error("out of wasm memory (fork)"); }
    child.state.u8.set(src);
    child.state.brk = parent.state.brk;
    child.state.nextFd = parent.state.nextFd;
    child.state.errnoPtr = parent.state.errnoPtr;
    child.asyncifyPtr = parent.asyncifyPtr;
    // Rewind the child to the fork callsite; it returns 0 there.
    child.forkReturn = 0;
    child.pendingRewind = true;
    this.run(child); // cooperative: child (and its subtree) runs to completion
    // Release the child's wasm instance + linear memory now that it has
    // exited — keep only pid/ppid/comm/exitCode for ps and waitpid.
    // Without this, a 111-fork tree allocates 111 live instances and the
    // browser runs out of wasm memory.
    child.inst = null; child.state = null;
    // Now resume the parent; fork returns the child pid.
    parent.forkReturn = child.pid;
    parent.pendingRewind = true;
  }

  waitpid(parent, pid, statusPtr) {
    const kid = this.procs.find((p) => p.ppid === parent.pid && p.exited && !p.reaped && (pid <= 0 || p.pid === pid));
    if (!kid) return -1; // ECHILD (no unreaped children — they ran cooperatively already)
    kid.reaped = true;
    if (statusPtr) parent.state.view.setUint32(statusPtr, ((kid.exitCode & 0xff) << 8) | (kid.signal & 0x7f), true);
    return kid.pid;
  }

  kill(pid, sig) { const p = this.procs.find((q) => q.pid === pid); if (p && !p.exited) { p.exited = true; p.signal = sig; p.exitCode = 128 + sig; } return 0; }

  // execve / nested program run: fresh root-like process tree, returns exit.
  runChildProgram(mod, argv, ppid) {
    const child = this.spawn(mod, argv, ppid, undefined);
    this.run(child);
    child.reaped = true;
    child.inst = null; child.state = null; // free the exec'd image's memory
    return { exitCode: child.exitCode };
  }
}

// Run a program (with optional tools for exec), returning its exit code.
// Same signature shape as the simple runner so callers swap easily.
export function runProgram(mod, argv, onOutput, onUnimpl, opts = {}) {
  const mgr = new Manager({ onOutput, onUnimpl: onUnimpl || (() => {}) }, opts.tools || new Map());
  const root = mgr.spawn(mod, argv, 0, opts.env);
  try { mgr.run(root); } catch (e) { return { exitCode: "trap", error: e.message, procs: mgr.procs.length }; }
  return { exitCode: typeof root.exitCode === "number" ? root.exitCode : 0, error: root.error, procs: mgr.procs.length };
}
