// Shared libc env for the multi-thread engine. Used by BOTH the
// coordinator (runs the main process + fork) and the pool workers (run
// thread functions). Everything operates on ONE shared linear memory.
//
// Key differences from the single-thread engine:
//  - the malloc bump pointer lives IN shared memory (Atomics) so the
//    main thread and all worker threads allocate without colliding;
//  - pthread_* are real: create → hand work to a pooled Worker, mutex/
//    cond/rwlock → Atomics futexes, join → Atomics.wait.
//
// Fixed control region near the top of the (pre-sized, non-growing)
// shared memory:
// 6 MiB per process — small enough that a 110-deep fork tree fits the
// browser's wasm budget when created in one synchronous burst (no GC
// between forks); large enough for the main process's 256 KiB phase-6
// buffer. Children barely touch the heap. Control region sits at the top.
export const MEM_PAGES = 48;                  // 3 MiB, never grown
export const ALLOC_PTR = 2 * 1024 * 1024 + 768 * 1024; // 2.75 MiB: shared bump pointer
export const POOL_BASE = 2 * 1024 * 1024 + 512 * 1024; // 2.5 MiB: pool slots
export const READY_OFF = POOL_BASE + 8 * 16;  // ready counter
export const FLAG_BASE = 2 * 1024 * 1024 + 256 * 1024; // 2.25 MiB: per-thread join flags
export const ASYNC_BUF = 2 * 1024 * 1024;     // 2 MiB: asyncify buffer for fork
export const ASYNC_SZ = 16384;
export const HEAP_START = 512 * 1024;         // heap base (above static data)
export const POOL_SIZE = 8;

const enc = new TextEncoder();
const dec = new TextDecoder();

export function makeMem() {
  return new WebAssembly.Memory({ initial: MEM_PAGES, maximum: MEM_PAGES, shared: true });
}

// One env, shared layout. `ctx` carries pid/ppid + the host hooks
// (spawnThread, fork, output). `view`/`u8` are rebuilt per call cheaply.
export function buildEnv(memory, ctx) {
  const v = new DataView(memory.buffer);
  const u8 = new Uint8Array(memory.buffer);
  const ia = new Int32Array(memory.buffer);
  const OK = (a) => a > 0 && (a >> 2) + 1 < ia.length; // valid atomic word address
  const cstr = (p) => { if (!p) return ""; let e = p; while (u8[e]) e++; return dec.decode(u8.slice(p, e)); };
  const alloc = (n, align = 8) => {
    // atomic bump: round up, reserve, return old top
    for (;;) {
      const cur = Atomics.load(ia, ALLOC_PTR >> 2);
      const base = (cur + (align - 1)) & ~(align - 1);
      if (Atomics.compareExchange(ia, ALLOC_PTR >> 2, cur, base + n) === cur) return base;
    }
  };
  const putStr = (s) => { const b = enc.encode(s + "\0"); const p = alloc(b.length, 1); u8.set(b, p); return p; };
  const fmt = (f, vp) => ctx.format(memory, f, vp);

  const fdOfFile = (fp) => (fp >= 1 && fp <= 3 ? fp - 1 : 1);
  const writeBytes = (fd, p, n) => ctx.write(fd, p, n);
  const emit = (fd, t) => ctx.emit(fd, t);

  const env = {
    memory,
    __main_argc_argv: (argc, argv) => ctx.callMain(argc, argv),
    __yos_argc: () => ctx.argv.length,
    __yos_argv_setup: (p) => { for (let i = 0; i < ctx.argv.length; i++) v.setUint32(p + i * 4, putStr(ctx.argv[i]), true); },
    __yos_envc: () => ctx.envp.length,
    __yos_envp_setup: (p) => { for (let i = 0; i < ctx.envp.length; i++) v.setUint32(p + i * 4, putStr(ctx.envp[i]), true); },
    exit: (c) => { const e = new Error("exit"); e.isExit = true; e.code = c | 0; throw e; },
    _exit: (c) => { const e = new Error("exit"); e.isExit = true; e.code = c | 0; throw e; },
    abort: () => { const e = new Error("abort"); e.isExit = true; e.code = 134; throw e; },

    malloc: (n) => alloc(n), calloc: (a, b) => { const p = alloc(a * b); u8.fill(0, p, p + a * b); return p; },
    realloc: (p, n) => { const q = alloc(n); if (p) u8.copyWithin(q, p, p + n); return q; }, free: () => {},
    memset: (d, c, n) => { u8.fill(c & 0xff, d, d + n); return d; }, bzero: (d, n) => { u8.fill(0, d, d + n); return 0; },
    memcpy: (d, s, n) => { u8.copyWithin(d, s, s + n); return d; }, memmove: (d, s, n) => { u8.copyWithin(d, s, s + n); return d; },
    __error: () => ctx.errnoPtr,
    getenv: (n) => { const k = cstr(n); const h = ctx.envp.find((e) => e.startsWith(k + "=")); return h ? putStr(h.slice(k.length + 1)) : 0; },
    setenv: () => 0, unsetenv: () => 0, setlocale: () => putStr("C"), nl_langinfo: () => putStr(""), ___mb_cur_max: () => 1,
    sysconf: () => -1, getuid: () => 0, geteuid: () => 0, getgid: () => 0, getegid: () => 0,
    getpid: () => ctx.pid, getppid: () => ctx.ppid, getpgrp: () => ctx.pid, getgroups: () => 0, isatty: () => 0,

    write: (fd, p, n) => writeBytes(fd, p, n),
    writev: (fd, iov, c) => { let t = 0; for (let i = 0; i < c; i++) { const b = v.getUint32(iov + i * 8, true); const l = v.getUint32(iov + i * 8 + 4, true); t += writeBytes(fd, b, l); } return t; },
    printf: (f, vp) => { const s = fmt(f, vp); emit(1, s); return s.length; },
    fprintf: (fp, f, vp) => { const s = fmt(f, vp); emit(fdOfFile(fp), s); return s.length; },
    vfprintf: (fp, f, vp) => { const s = fmt(f, vp); emit(fdOfFile(fp), s); return s.length; },
    snprintf: (d, n, f, vp) => { const s = fmt(f, vp); const b = enc.encode(s).subarray(0, Math.max(0, n - 1)); u8.set(b, d); u8[d + b.length] = 0; return s.length; },
    vsnprintf: (d, n, f, vp) => env.snprintf(d, n, f, vp),
    sprintf: (d, f, vp) => { const s = fmt(f, vp); const b = enc.encode(s); u8.set(b, d); u8[d + b.length] = 0; return s.length; },
    fputc: (c, fp) => { emit(fdOfFile(fp), String.fromCharCode(c & 0xff)); return c & 0xff; }, putc: (c, fp) => env.fputc(c, fp),
    fputs: (s, fp) => { emit(fdOfFile(fp), cstr(s)); return 1; }, puts: (s) => { emit(1, cstr(s) + "\n"); return 1; },
    fwrite: (p, sz, nm, fp) => { writeBytes(fdOfFile(fp), p, sz * nm); return nm; },
    fflush: () => 0, fileno: (fp) => fp, setvbuf: () => 0, setbuf: () => 0, __swbuf: (c, fp) => { emit(fdOfFile(fp), String.fromCharCode(c & 0xff)); return c & 0xff; },
    clearerr: () => 0, ferror: () => 0, feof: () => 0,

    strlen: (p) => { let n = 0; while (u8[p + n]) n++; return n; },
    strcmp: (a, b) => { let i = 0; for (;;) { const x = u8[a + i], y = u8[b + i]; if (x !== y) return x - y; if (!x) return 0; i++; } },
    strncmp: (a, b, n) => { for (let i = 0; i < n; i++) { const x = u8[a + i], y = u8[b + i]; if (x !== y) return x - y; if (!x) return 0; } return 0; },
    strchr: (s, c) => { c &= 0xff; for (let p = s; ; p++) { if (u8[p] === c) return p; if (!u8[p]) return c === 0 ? p : 0; } },
    strrchr: (s, c) => { c &= 0xff; let hit = 0; for (let p = s; ; p++) { if (u8[p] === c) hit = p; if (!u8[p]) return c === 0 ? p : hit; } },
    strcpy: (d, s) => { let i = 0; do { u8[d + i] = u8[s + i]; } while (u8[s + i++]); return d; },
    strncpy: (d, s, n) => { let i = 0; for (; i < n && u8[s + i]; i++) u8[d + i] = u8[s + i]; for (; i < n; i++) u8[d + i] = 0; return d; },
    strcat: (d, s) => { env.strcpy(d + env.strlen(d), s); return d; }, strdup: (s) => putStr(cstr(s)),
    strstr: (h, n) => { const ndl = cstr(n); if (!ndl) return h; const hay = cstr(h); const i = hay.indexOf(ndl); return i < 0 ? 0 : h + enc.encode(hay.slice(0, i)).length; },
    memcmp: (a, b, n) => { for (let i = 0; i < n; i++) { const x = u8[a + i], y = u8[b + i]; if (x !== y) return x - y; } return 0; },
    memchr: (s, c, n) => { c &= 0xff; for (let i = 0; i < n; i++) if (u8[s + i] === c) return s + i; return 0; },
    strtoul: (s, endp, base) => { const str = cstr(s); const m = str.match(/^\s*[+-]?(0x[0-9a-fA-F]+|[0-9]+)/); const val = m ? parseInt(m[0], base || (m[0].includes("0x") ? 16 : 10)) >>> 0 : 0; if (endp) v.setUint32(endp, s + (m ? m[0].length : 0), true); return val; },
    strtol: (s, endp, base) => env.strtoul(s, endp, base) | 0,
    atoi: (s) => parseInt(cstr(s), 10) || 0,
    strerror: (n) => putStr("Error " + n), qsort: () => 0,
    abs: (x) => Math.abs(x | 0), rand: () => (Atomics.add(ia, (ALLOC_PTR - 8) >> 2, 1103515245) >>> 1), srand: () => 0,

    // time
    time: (t) => { const s = Math.floor(ctx.now() / 1000); if (t) v.setUint32(t, s, true); return s; },
    gettimeofday: (tv) => { const ms = ctx.now(); if (tv) { v.setUint32(tv, Math.floor(ms / 1000), true); v.setUint32(tv + 4, (ms % 1000) * 1000, true); } return 0; },
    clock_gettime: (id, ts) => { const ms = ctx.now(); if (ts) { v.setUint32(ts, Math.floor(ms / 1000), true); v.setUint32(ts + 4, Math.floor((ms % 1000) * 1e6), true); } return 0; },
    nanosleep: () => 0, sleep: () => 0, usleep: () => 0,
    localtime: () => 0, mktime: () => 0, strftime: (d) => { u8[d] = 0; return 0; },
    gethostname: (b, l) => { u8.set(enc.encode("yos-web".slice(0, l - 1) + "\0"), b); return 0; },

    // process: fork via asyncify (host hook), exec, wait, signals
    fork: () => ctx.fork(), vfork: () => ctx.fork(),
    waitpid: (pid, st) => ctx.waitpid(pid, st), wait3: (st) => ctx.waitpid(-1, st), wait4: (pid, st) => ctx.waitpid(pid, st),
    execve: (p, a) => ctx.execve(cstr(p), a), kill: (pid, sig) => ctx.kill(pid, sig), killpg: () => 0,
    getrusage: () => 0, getrlimit: () => 0, setrlimit: () => 0, times: () => 0, nice: () => 0, setpgid: () => 0, getlogin: () => 0,
    signal: () => 0, sigaction: () => 0, sigemptyset: () => 0, sigfillset: () => 0, sigaddset: () => 0, sigprocmask: () => 0, sigsuspend: () => 0, alarm: () => 0, raise: () => 0, sysctl: (a, b, c, d) => ctx.sysctl(a, b, c, d),

    // VFS (host hook)
    open: (p, fl) => ctx.open(cstr(p), fl), openat: (d, p, fl) => ctx.open(cstr(p), fl), close: (fd) => ctx.close(fd),
    read: (fd, b, n) => ctx.read(fd, b, n), lseek: (fd, o, w) => ctx.lseek(fd, o, w),
    stat: (p, b) => ctx.stat(cstr(p), b), lstat: (p, b) => ctx.stat(cstr(p), b), fstat: (fd, b) => ctx.fstat(fd, b), fstatat: (d, p, b) => ctx.stat(cstr(p), b),
    fstatfs: (fd, b) => { u8.fill(0, b, b + 256); return 0; }, statfs: () => 0, access: (p) => ctx.access(cstr(p)), unlink: (p) => ctx.unlink(cstr(p)),
    fcntl: () => 0, dup: () => ctx.dupfd(), dup2: (a, b) => b, pipe: (p) => { v.setUint32(p, ctx.dupfd(), true); v.setUint32(p + 4, ctx.dupfd(), true); return 0; },
    opendir: (p) => ctx.opendir(cstr(p)), fdopendir: (fd) => ctx.fdopendir(fd), readdir: (h) => ctx.readdir(h), closedir: (h) => ctx.close(h), dirfd: (h) => h,
    getcwd: (b) => { u8.set(enc.encode("/\0"), b); return b; }, chdir: () => 0, fchdir: () => 0, umask: () => 0o22, mkdir: () => 0, rmdir: () => 0, readlink: () => -1, realpath: (p, o) => { u8.set(enc.encode(cstr(p) + "\0"), o); return o; },
    ttyname: () => 0, tcgetattr: () => -1, tcsetattr: () => 0, ioctl: () => -1, poll: () => 0, select: () => 0,
    mmap: () => -1, munmap: () => 0, mprotect: () => 0, madvise: () => 0,
    mbrtowc: (pw, s) => { if (!s) return 0; const c = u8[s]; if (pw) v.setUint32(pw, c, true); return c ? 1 : 0; }, wcwidth: () => 1,

    // --- REAL threads ---
    pthread_create: (out, attr, fnIdx, arg) => ctx.threadCreate(out, fnIdx, arg),
    pthread_join: (tid) => ctx.threadJoin(tid), pthread_detach: () => 0, pthread_self: () => ctx.pid, pthread_exit: () => { const e = new Error("texit"); e.isThreadExit = true; throw e; },
    pthread_mutex_init: (m) => { if (OK(m)) Atomics.store(ia, m >> 2, 0); return 0; }, pthread_mutex_destroy: () => 0,
    pthread_mutex_lock: (m) => { if (!OK(m)) return 0; const i = m >> 2; for (;;) { if (Atomics.compareExchange(ia, i, 0, 1) === 0) return 0; Atomics.wait(ia, i, 1, 2000); } },
    pthread_mutex_unlock: (m) => { if (!OK(m)) return 0; const i = m >> 2; Atomics.store(ia, i, 0); Atomics.notify(ia, i, 1); return 0; },
    pthread_mutex_trylock: (m) => (OK(m) && Atomics.compareExchange(ia, m >> 2, 0, 1) === 0 ? 0 : 16),
    pthread_cond_init: (c) => { if (OK(c)) Atomics.store(ia, c >> 2, 0); return 0; }, pthread_cond_destroy: () => 0,
    pthread_cond_signal: (c) => { if (!OK(c)) return 0; const i = c >> 2; Atomics.add(ia, i, 1); Atomics.notify(ia, i, 1); return 0; },
    pthread_cond_broadcast: (c) => { if (!OK(c)) return 0; const i = c >> 2; Atomics.add(ia, i, 1); Atomics.notify(ia, i, 1 << 30); return 0; },
    // cond_wait: release mutex, wait for the cond seq to change, re-lock.
    pthread_cond_wait: (c, m) => { if (!OK(c)) return 0; const ci = c >> 2; const seq = Atomics.load(ia, ci); env.pthread_mutex_unlock(m); Atomics.wait(ia, ci, seq, 5000); env.pthread_mutex_lock(m); return 0; },
    pthread_rwlock_init: (l) => { if (OK(l)) Atomics.store(ia, l >> 2, 0); return 0; }, pthread_rwlock_destroy: () => 0,
    // simple rwlock: word>=0 readers, -1 writer. Atomics.
    pthread_rwlock_rdlock: (l) => { if (!OK(l)) return 0; const i = l >> 2; for (;;) { const c = Atomics.load(ia, i); if (c >= 0 && Atomics.compareExchange(ia, i, c, c + 1) === c) return 0; Atomics.wait(ia, i, c, 2000); } },
    pthread_rwlock_wrlock: (l) => { if (!OK(l)) return 0; const i = l >> 2; for (;;) { if (Atomics.compareExchange(ia, i, 0, -1) === 0) return 0; Atomics.wait(ia, i, Atomics.load(ia, i), 2000); } },
    pthread_rwlock_unlock: (l) => { if (!OK(l)) return 0; const i = l >> 2; const c = Atomics.load(ia, i); if (c === -1) Atomics.store(ia, i, 0); else Atomics.sub(ia, i, 1); Atomics.notify(ia, i, 1 << 30); return 0; },
    pthread_key_create: () => 0, pthread_key_delete: () => 0, pthread_setspecific: () => 0, pthread_getspecific: () => 0, pthread_once: (o, fnIdx) => { const i = o >> 2; if (Atomics.compareExchange(ia, i, 0, 1) === 0) ctx.callIndirect(fnIdx, 0); return 0; },
    pthread_attr_init: () => 0, pthread_attr_destroy: () => 0, pthread_attr_setdetachstate: () => 0, pthread_attr_setstacksize: () => 0,
  };

  // Any import we forgot → logged no-op (so a missing fn doesn't trap).
  return new Proxy(env, { get(t, k) { if (k in t) return t[k]; if (typeof k === "string" && k !== "memory") { return (...a) => { ctx.unimpl && ctx.unimpl(k); return 0; }; } return t[k]; } });
}
