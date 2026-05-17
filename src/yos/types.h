#ifndef YOS_TYPES_H
#define YOS_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <limits.h>      /* PATH_MAX from POSIX libc, not Linux UAPI */
#include "yos_autoglobals.h"   /* generated: struct yos_autoglobals */

/* Asyncify states (from Binaryen's asyncify transformation) */
#define ASYNCIFY_NORMAL    0
#define ASYNCIFY_UNWINDING 1
#define ASYNCIFY_REWINDING 2

/* Asyncify buffer size (reused for each fork) */
#define ASYNCIFY_BUF_SIZE  16384

/* Max processes in table */
#define YOS_MAX_PROCS      64

/* Max free regions for mmap reuse */
#define YOS_MAX_FREE_REGIONS 64

/* Freed memory region (for mmap reuse in WASM linear memory) */
struct yos_free_region {
    uint32_t addr;
    uint32_t len;
};

/* Process states */
typedef enum {
    YOS_PROC_FREE = 0,
    YOS_PROC_READY,
    YOS_PROC_RUNNING,
    YOS_PROC_ZOMBIE,
} yos_proc_state_t;

/* Process identity (slot in process table) */
struct yos_proc {
    int32_t pid;
    int32_t ppid;
    int32_t pgid;
    int32_t sid;
    /* Thread-group id. For threads spawned via clone(CLONE_THREAD), this
     * is the tgid of the calling task — getpid() returns proc->tgid,
     * gettid() returns proc->pid, so all threads of a task agree on
     * getpid(). For everything else (init, fork, vfork) tgid == pid.
     * Default-initialised to pid in yos_proc_alloc. */
    int32_t tgid;
    /* CLONE_CHILD_CLEARTID address (wasm offset). On thread/proc exit
     * the worker atomically writes 0 here and futex-wakes any waiter,
     * which is what unblocks pthread_join. set_tid_address(2) updates
     * this late-bound; CLONE_CHILD_CLEARTID sets it at clone time. */
    uint32_t tid_address;
    yos_proc_state_t state;
    int32_t exit_code;

    pthread_mutex_t lock;
    pthread_cond_t wait_cond;
    int exited;

    /* vfork support: parent blocks until child exec/_exit */
    int32_t vfork_parent_pid;
    pthread_cond_t vfork_cond;
    int vfork_child_done;

    pthread_t thread;

    /* Process info for /proc/[pid] (stored per-process, not per-ctx) */
    char comm[16];           /* command name (basename of exe) */
    char exe[PATH_MAX];      /* path to executable */
    char cwd[PATH_MAX];      /* current working directory */
    char **cmdline;          /* command line args (NULL-terminated) */
    int cmdline_argc;
};

struct yos_runtime;  /* forward decl */

struct yos_exec_ctx {
    /* Links */
    struct yos_runtime *rt;
    struct yos_proc *proc;

    /* WASM state */
    uint8_t *memory;      /* wasm linear memory base */
    uint32_t memory_size; /* wasm linear memory size */
    uint32_t heap_end;    /* brk position — what brk(0) reports back */
    /* Separate watermark for anonymous mmap2 allocations. wasm_brk
     * (heap_end above) can shrink (musl gives memory back to the kernel
     * in the oldmalloc __bin_chunk path), but mmap allocations above
     * the new brk are still LIVE. If mmap2 also used heap_end, a brk
     * shrink would let a later mmap2 hand out memory that was already
     * given to a prior mmap2 — silent heap corruption. So keep mmap2
     * on its own monotonically-rising cursor. */
    uint32_t mmap_top;
    void *runtime;        /* IM3Runtime */
    void *module;         /* IM3Module */
    uint8_t *wasm_bytes;  /* raw wasm binary (kept for fork) */
    size_t wasm_bytes_size;

    /* Memory free list (for mmap reuse) */
    struct yos_free_region free_list[YOS_MAX_FREE_REGIONS];
    int free_count;
    pthread_mutex_t mem_lock;

    /* Command line */
    int argc;
    char **argv;
    /* Environment passed to wasm. envc is the number of strings; envp
     * is a NULL-terminated array of host-side strdup'd strings. NULL
     * envp means "no environment" (treated as empty for crt1 purposes). */
    int envc;
    char **envp;

    /* Fork state (asyncify-based) */
    uint32_t asyncify_ptr;
    int fork_pending;
    int32_t fork_return;
    int is_child;

    /* AUTO-isolated libc globals — bridge.py emits the per-ctx
     * field definitions for everything marked auto_save_restore in
     * the policy file. yos_autoglobals.h is generated; if you don't
     * see your global here at runtime, check whether it's listed.
     * The header path resolves through codegen's include dir. */
    struct yos_autoglobals autoglobals;

    /* Per-ctx libc-globals isolation (build-tools/libbridge/policies/libc.yaml).
     *
     * Each field below replaces a SHARED host-libc global that, left
     * unguarded, would let guest A's call corrupt guest B's view.
     * The bridges (impl/getopt.c, impl/tz.c, …) read+write these
     * ctx fields instead of touching the host global directly.
     *
     * Adding a new entry here = corresponding policy.yaml entry MUST
     * be promoted from `leaks` to `bridged_per_ctx` with `via:` =
     * the impl file. The extractor's --fail-on-leak catches gaps. */

    /* getopt state (optind/optarg/optopt/opterr). impl/getopt.c
     * implements the parser itself (FreeBSD-derived) and never calls
     * host getopt — eliminates the host-libc-global write entirely.
     * `optind = 1` is the POSIX initial value; opterr defaults to 1
     * (print error messages). */
    struct {
        int   optind;       /* next argv index to inspect; init 1 */
        int   opterr;       /* if non-zero, print errors; init 1   */
        int   optopt;       /* the unrecognized opt char            */
        uint32_t optarg_off;/* wasm-memory offset of current optarg */
    } getopt_state;

    /* Timezone state. tzset() with $TZ set mutates host tzname/
     * timezone/daylight. impl/tz.c swaps them in from this ctx slot
     * before any timezone-sensitive call (localtime/mktime/strftime),
     * swaps host's previous state out after.
     * Initialised lazily: `initialized=0` until first tzset/localtime. */
    struct {
        int   initialized;
        char  tzname0[64];      /* tzname[0] copy (e.g. "CET")     */
        char  tzname1[64];      /* tzname[1] copy (e.g. "CEST")    */
        long  timezone;         /* seconds west of UTC             */
        int   daylight;         /* 1 if DST observed in this zone  */
    } tz_state;

    /* Resolver state — deferred. Adding requires bridging
     * gethostbyname/getaddrinfo via res_n* reentrant variants.
     * Until then env.gethostbyname is stubbed in the auto-bridge. */
    void *resolver_state;       /* future: struct __res_state * */

    /* Locale state — current `setlocale` argument per-ctx. impl/pwd.c
     * currently calls host setlocale directly (leak); to be fixed by
     * holding the locale string here and re-applying via uselocale
     * (per-thread, glibc) on each bridge entry. */
    char locale_name[64];       /* "" = host default */

    /* Per-guest bridged-library state.
     *
     * yos hosts arbitrary native libraries (libpython3.12, future:
     * libsqlite, libssl, ...) and exposes their C APIs to wasm guests
     * via env.* bridges (impl/libpython.c et al). Each guest gets its
     * own slice of every library's state — held here, owned by the
     * library bridge, opaque to the rest of yos. NULL until the guest
     * calls the library's init bridge.
     *
     * py_tstate: PyThreadState * of this guest's CPython subinterpreter.
     *   Subinterpreters give each guest its own sys.modules / builtins /
     *   (3.12+) GIL — without them two guests sharing one libpython
     *   would see each other's monkey-patches and global mutation.
     *   See impl/libpython.c. */
    void *py_tstate;

    /* "Did this ctx write to stderr (wfd=2) since the last failed exec?"
     * Used by yos_exit to detect a forked child that died after exec
     * failure without printing — under asyncify-fork, zsh's zwarning code
     * path doesn't reach env.fputc in the child (the *parent* path prints
     * fine). Yos synthesises the diagnostic itself in that case so the
     * user sees something instead of dead silence. Bumped by yos_write/
     * yos_fputc/yos_fwrite/yos_fputs/yos_vfprintf when fd/handle resolves
     * to wfd 2; reset by yos_execve on each attempt. last_failed_exec_*
     * remember the most recent ENOENT-class execve so we can format it. */
    int stderr_written_since_exec;
    char last_failed_exec_path[256];
    int last_failed_exec_errno;

    /* setjmp/longjmp state (asyncify-based, see m3_setjmp/m3_longjmp).
     *
     * Each LIVE setjmp has its own slot in sj_slots[] keyed by the user's
     * jmp_buf address. Per-slot we keep:
     *   - asyncify_buf:    wasm-side buffer asyncify_start_unwind writes to
     *   - save_data:       host-side snapshot of that buffer (rewind
     *                      consumes the buffer as it replays, so each
     *                      longjmp has to restore it from this copy)
     *
     * Why per-slot, not a single global: an inner setjmp would overwrite
     * an outer setjmp's saved state. Test 5 in tests/ut/yos/test_setjmp.c
     * (outer-longjmp skips inner) is the regression that pinned this.
     *
     * sj_discard_ptr is shared scratch — longjmp unwinds INTO it just to
     * leave the wasm call stack, then the pump throws it away.
     *
     * setjmp_pending is set on setjmp's first call to flag the unwind→
     * rewind round-trip the main pump must drive (so setjmp returns 0
     * with proper stack state captured). longjmp_pending + longjmp_value
     * + longjmp_target drive the rewind from the matching slot's
     * asyncify_buf; m3_setjmp returns longjmp_value when its own
     * jmp_buf_ptr equals longjmp_target. */
    struct yos_sj_slot {
        uint32_t jmp_buf_ptr;        /* user's jmp_buf addr; 0 = unused */
        uint32_t asyncify_buf;       /* wasm asyncify save buffer addr */
        void    *save_data;          /* host snapshot of that buffer */
        size_t   save_size;
    } sj_slots[64];
    /* Index of the slot whose first-call unwind is in flight (-1 = none).
     * set by m3_setjmp before asyncify_start_unwind, consumed by the pump. */
    int      setjmp_pending_slot;
    uint32_t sj_discard_ptr;
    int      setjmp_pending;        /* legacy: == (setjmp_pending_slot>=0) */
    int      longjmp_pending;
    int32_t  longjmp_value;
    /* Trap result captured from inside yos_setjmp_pump's _start invocation
     * so the caller can detect that the wasm trapped during a rewind step
     * (the pump otherwise discards the m3_CallV return). NULL if no trap. */
    const char *pump_trap;
    /* User's jmp_buf the active longjmp is targeting. m3_setjmp matches
     * its own arg against this so an inner setjmp doesn't falsely consume
     * an outer setjmp's longjmp. 0 means "no longjmp in flight". */
    uint32_t longjmp_target;

    /* Exec state */
    int exec_pending;
    char exec_path[PATH_MAX];
    int exec_argc;
    char **exec_argv;  /* heap-allocated, freed after exec */
    int exec_envc;
    char **exec_envp;  /* heap-allocated, NULL means "inherit ctx->envp" */

    /* Virtual file table for procfs etc. (allocated on first use) */
    void *procfs_fds;

    /* Per-runtime fd table. fd_map[wasm_fd] = host_fd; -1 means slot
     * unused (wasm fd not open). Every fd-allocating syscall (open /
     * socket / pipe / dup2 / accept4 / epoll_create1 / …) returns a
     * wasm fd allocated from this table; every fd-consuming syscall
     * translates wasm→host before invoking the kernel. Fork dups each
     * used host fd so parent and child have independent host fds for
     * the same wasm fd numbers, which is the only way to support
     * fork-as-thread without one runtime's close/dup2 trampling the
     * other's fds. wfd 0/1/2 default to host 0/1/2 (inherited stdio);
     * other slots are -1 until allocated. Virtual fds (procfs etc.)
     * use wfd ≥ YOS_VFS_FD_BASE and skip this table. */
#define YOS_FD_MAX 256
    int fd_map[YOS_FD_MAX];

    /* Current working directory (tracked for virtual paths) */
    char cwd[PATH_MAX];

    /* Per-runtime POSIX timer_t handle table. Host timer_t is `void *`
     * (8 bytes); wasm32 stores the timer id in 4 bytes — we hand the
     * wasm side a small int32 index and look up the real `void *` here
     * before each timer_settime/gettime/delete. Kept per-ctx so a
     * forked child gets its own table; a global one would let one
     * process delete or read timers another owns. 0 means slot free. */
#define YOS_TIMER_MAX 64
    void *timer_ids[YOS_TIMER_MAX];
    pthread_mutex_t timer_lock;
    int             timer_lock_init;

    /* Wasm offset of the int-sized slot that backs the FreeBSD `errno`
     * macro (#define errno (*__error())). yos___error(ctx) returns this
     * offset; bridges write here on error paths so client code's
     * `errno` reads the right value. Per-ctx for now (single-thread
     * correct); pthread workers need their own slot — TODO. */
    uint32_t errno_off;

    /* Guest-facing allocator state (impl/alloc.c).
     *
     * Pure free-list allocator inside [alloc_lo, alloc_hi) of the
     * guest's wasm linear memory. State (free-list head + block
     * headers) lives IN ctx->memory itself, so on execve the new
     * runtime's fresh linear memory starts with a clean allocator
     * automatically — no host-side global registry to dangle, no
     * mimalloc, no per-ctx cleanup needed. Lazy-inited on first
     * malloc by impl/alloc.c.
     *
     *   alloc_lo, alloc_hi        — wasm-offset bounds of the heap
     *                               region. Zero = not initialised.
     *   alloc_free_head           — wasm offset of the first free
     *                               block, or 0 if the list is empty.
     */
    uint32_t alloc_lo;
    uint32_t alloc_hi;
    uint32_t alloc_free_head;
};

/* Global runtime state */
struct yos_runtime {
    struct yos_proc procs[YOS_MAX_PROCS];
    pthread_mutex_t proc_lock;
    /* Broadcast every time ANY proc transitions to ZOMBIE — main.c
     * waits on it during shutdown so yos doesn't exit while a
     * forked child is still running. Per-proc wait_cond is for
     * waitpid() consumers (one cond var per child); this one is
     * a runtime-wide "something exited" event. */
    pthread_cond_t  any_exit_cond;
    int32_t next_pid;

    /* Foreground process-group of the controlling tty, virtualized in
     * the guest namespace. TIOCGPGRP/TIOCSPGRP on a tty fd read/write
     * this instead of the host kernel's value (which would be the host
     * shell's pgrp, an unrelated number from the guest's perspective).
     * Initialized to 1 = init proc, so ash's foreground-pgrp loop
     * (compares getpgrp()==tcgetpgrp()) succeeds at startup. */
    int32_t fg_pgid;

    int argc;
    char **argv;
    int envc;
    char **envp;

    /* VFS mount table */
    void *mount_table;

    /* Lazy-allocated host-side L1 pthread implementation. Holds the per-process
     * thread-slot table + TLS arena pool. yos_link_imports() asks for it so
     * every loaded module gets the yos_pthread_* imports bound. Treated as
     * `struct yos_pthread_host *` by code that includes "impl/pthread.h"; kept
     * void here to avoid pulling that header into every translation unit. */
    void *pthread_host;
};

#endif /* YOS_TYPES_H */
