#define _GNU_SOURCE
#define _GNU_SOURCE   /* for the syscall() prototype in <unistd.h> */
#include "yos/types.h"
#include "yos/ydebug.h"
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/syscall.h>
/* CLEARTID exit-wake done via pthread_cond_broadcast on the proc's
 * wait_cond (see below). No Linux futex syscall needed on the host;
 * any guest libc that uses futex semantics goes through a small
 * portable shim. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "wasm3.h"
#include "m3_env.h"

/* ============================================================================
 * Asyncify Helpers
 * ============================================================================ */

static void call_asyncify(IM3Runtime rt, const char *name, uint32_t arg)
{
    IM3Function f;
    if (m3_FindFunction(&f, rt, name) == NULL) {
        if (arg != (uint32_t)-1)
            m3_CallV(f, arg);
        else
            m3_CallV(f);
    }
}

static int get_asyncify_state(IM3Runtime rt)
{
    IM3Function f;
    if (m3_FindFunction(&f, rt, "asyncify_get_state"))
        return -1;
    m3_CallV(f);
    int32_t state;
    m3_GetResultsV(f, &state);
    return state;
}

/* ============================================================================
 * Process Table Operations
 * ============================================================================ */

struct yos_proc *yos_proc_alloc(struct yos_runtime *rt, int32_t ppid)
{
    pthread_mutex_lock(&rt->proc_lock);

    for (int i = 0; i < YOS_MAX_PROCS; i++) {
        if (rt->procs[i].state == YOS_PROC_FREE) {
            struct yos_proc *p = &rt->procs[i];
            memset(p, 0, sizeof(*p));
            p->pid = rt->next_pid++;
            p->ppid = ppid;
            p->pgid = p->pid;
            p->sid = p->pid;
            p->tgid = p->pid;   /* default; clone(CLONE_THREAD) overrides */
            p->tid_address = 0;
            p->state = YOS_PROC_READY;
            p->vfork_parent_pid = -1;
            pthread_mutex_init(&p->lock, NULL);
            pthread_cond_init(&p->wait_cond, NULL);
            pthread_cond_init(&p->vfork_cond, NULL);

            pthread_mutex_unlock(&rt->proc_lock);
            ydebug("allocated pid=%d ppid=%d\n", p->pid, ppid);
            return p;
        }
    }

    pthread_mutex_unlock(&rt->proc_lock);
    return NULL;
}

struct yos_proc *yos_proc_find(struct yos_runtime *rt, int32_t pid)
{
    pthread_mutex_lock(&rt->proc_lock);

    for (int i = 0; i < YOS_MAX_PROCS; i++) {
        if (rt->procs[i].pid == pid && rt->procs[i].state != YOS_PROC_FREE) {
            pthread_mutex_unlock(&rt->proc_lock);
            return &rt->procs[i];
        }
    }

    pthread_mutex_unlock(&rt->proc_lock);
    return NULL;
}

int32_t yos_exit(struct yos_exec_ctx *ctx, int32_t code)
{
    ydebug("exit(%d)\n", code);
    if (ctx->proc) {
        /* Signal vfork parent if applicable */
        if (ctx->proc->vfork_parent_pid > 0) {
            struct yos_proc *parent = yos_proc_find(ctx->rt, ctx->proc->vfork_parent_pid);
            if (parent) {
                pthread_mutex_lock(&parent->lock);
                parent->vfork_child_done = 1;
                pthread_cond_signal(&parent->vfork_cond);
                pthread_mutex_unlock(&parent->lock);
            }
            ctx->proc->vfork_parent_pid = -1;
        }

        /* CLONE_CHILD_CLEARTID / set_tid_address: zero the wasm-side
         * tid word. The host-side wakeup happens via the
         * `wait_cond` broadcast a few lines below, which is what
         * pthread_join uses on this side. Guest-side futex semantics
         * (if a guest's libc uses them) go through impl/futex.c. */
        if (ctx->proc->tid_address && ctx->memory &&
            ctx->proc->tid_address + 4 <= ctx->memory_size) {
            uint32_t *tid_word = (uint32_t *)(ctx->memory + ctx->proc->tid_address);
            __atomic_store_n(tid_word, 0u, __ATOMIC_SEQ_CST);
        }

        pthread_mutex_lock(&ctx->proc->lock);
        ctx->proc->state = YOS_PROC_ZOMBIE;
        ctx->proc->exit_code = code;
        ctx->proc->exited = 1;
        pthread_cond_broadcast(&ctx->proc->wait_cond);
        pthread_mutex_unlock(&ctx->proc->lock);
    }

    /* Forked children run in separate threads - only terminate the thread */
    if (ctx->is_child) {
        pthread_exit((void *)(intptr_t)code);
    }

    /* Main process - terminate the whole program */
    exit(code);
    return 0; /* unreachable */
}

/* ============================================================================
 * Fork Implementation (asyncify-based)
 * ============================================================================ */

int32_t yos_fork(struct yos_exec_ctx *ctx)
{
    fprintf(stderr, "*** YOS_FORK CALLED ***\n");
    if (!ctx->proc || !ctx->rt) {
        ydebug("fork: invalid context\n");
        return -EINVAL;
    }

    IM3Runtime wrt = (IM3Runtime)ctx->runtime;
    IM3Module mod = (IM3Module)ctx->module;

    if (!wrt || !mod) {
        ydebug("fork: no wasm runtime\n");
        return -ENOMEM;
    }

    int state = get_asyncify_state(wrt);

    /* If asyncify not available, fork cannot work */
    if (state < 0) {
        ydebug("fork: asyncify not available - WASM must be compiled with asyncify\n");
        return -ENOSYS;
    }

    /* REWIND path: returning from fork after rewind */
    if (state == ASYNCIFY_REWINDING) {
        call_asyncify(wrt, "asyncify_stop_rewind", -1);
        ydebug("fork rewind complete, returning %d\n", ctx->fork_return);
        return ctx->fork_return;
    }

    /* FIRST CALL path: trigger unwind */
    ydebug("fork called by pid=%d, triggering unwind\n", ctx->proc->pid);

    /* Per-runtime fd-table fork happens later in fork_thread_func once
     * the child ctx exists; nothing to do here. */

    /* Get memory for asyncify buffer */
    uint32_t mem_size;
    uint8_t *mem = m3_GetMemory(wrt, &mem_size, 0);
    if (!mem || mem_size == 0) {
        ydebug("fork: no wasm memory\n");
        return -ENOMEM;
    }

    /* Allocate asyncify buffer once (at end of memory), reuse for all forks */
    if (ctx->asyncify_ptr == 0) {
        ctx->asyncify_ptr = mem_size - ASYNCIFY_BUF_SIZE;
        ydebug("asyncify buffer at %u\n", ctx->asyncify_ptr);
    }

    /* Reset asyncify buffer header for this fork */
    uint32_t *buf = (uint32_t *)(mem + ctx->asyncify_ptr);
    buf[0] = ctx->asyncify_ptr + 8;
    buf[1] = ctx->asyncify_ptr + ASYNCIFY_BUF_SIZE;

    /* Allocate child process slot */
    struct yos_proc *child_proc = yos_proc_alloc(ctx->rt, ctx->proc->pid);
    if (!child_proc) {
        return -EAGAIN;
    }
    child_proc->pgid = ctx->proc->pgid;
    child_proc->sid = ctx->proc->sid;

    /* Store child pid as return value for parent (child gets 0) */
    ctx->fork_return = child_proc->pid;
    ctx->fork_pending = 1;

    ydebug("starting unwind, child will be pid=%d\n", child_proc->pid);
    call_asyncify(wrt, "asyncify_start_unwind", ctx->asyncify_ptr);

    return child_proc->pid;
}

/* Thread argument for child process */
typedef struct {
    struct yos_runtime *rt;
    struct yos_proc *proc;
    uint8_t *memory_snapshot;
    size_t memory_size;
    uint32_t asyncify_ptr;
    /* TODO(setjmp-refactor): ctx now has sj_slots[16] instead of a single
     * sj_asyncify_ptr — fork's setjmp-state handoff needs reworking to
     * carry all live slots. Leaving sj_discard_ptr only for now; nvim
     * doesn't fork, so no regression on the path being investigated. */
    uint32_t sj_discard_ptr;
    int64_t *wasm_globals;
    uint32_t wasm_globals_count;
    uint32_t heap_end;
    int argc;
    char **argv;
    int envc;
    char **envp;
    uint8_t *wasm_bytes;
    size_t wasm_bytes_size;
    /* Snapshot of parent's fd_map; child duplicates each entry to a
     * fresh host fd at startup so the runtimes have independent
     * close/dup2 semantics. */
    int parent_fd_map[YOS_FD_MAX];
    /* Parent's tracked cwd. Without this the child's ctx->cwd is the
     * calloc'd zero-string and getcwd returns nothing useful — it also
     * decouples relative-path lookup from the host process cwd, which
     * is shared across forks (they're all pthreads of one host pid). */
    char parent_cwd[PATH_MAX];
} fork_thread_arg_t;

static void *fork_thread_func(void *arg)
{
    fork_thread_arg_t *fork_thread_arg = (fork_thread_arg_t *)arg;

    /* Create new wasm3 environment and runtime for child */
    IM3Environment env = m3_NewEnvironment();
    IM3Runtime rt = m3_NewRuntime(env, 64 * 1024, NULL);
    if (!rt) {
        free(fork_thread_arg->memory_snapshot);
        free(fork_thread_arg->wasm_globals);
        free(fork_thread_arg);
        return NULL;
    }

    /* Create child exec context */
    struct yos_exec_ctx *child_ctx = calloc(1, sizeof(struct yos_exec_ctx));
    if (!child_ctx) {
        m3_FreeRuntime(rt);
        m3_FreeEnvironment(env);
        free(fork_thread_arg->memory_snapshot);
        free(fork_thread_arg->wasm_globals);
        free(fork_thread_arg);
        return NULL;
    }

    child_ctx->rt = fork_thread_arg->rt;
    child_ctx->proc = fork_thread_arg->proc;
    /* Record this thread on the child proc so kill(child_pid)/tkill in
     * the guest namespace can resolve the guest pid back to a real
     * pthread via deliver_to_proc(). */
    if (child_ctx->proc) child_ctx->proc->thread = pthread_self();
    child_ctx->runtime = rt;
    child_ctx->heap_end = fork_thread_arg->heap_end;
    child_ctx->asyncify_ptr = fork_thread_arg->asyncify_ptr;
    child_ctx->sj_discard_ptr  = fork_thread_arg->sj_discard_ptr;
    /* TODO(setjmp-refactor): copy parent's sj_slots[] into child. */
    child_ctx->fork_return = 0;  /* child gets 0 from fork */
    child_ctx->is_child = 1;
    /* Give the child its own host fds for each of the parent's open
     * wasm fds, so close/dup2 in one runtime doesn't trample the
     * other's. POSIX fork preserves FD_CLOEXEC; F_DUPFD strips it,
     * so re-set it on the dup when the source had it. The signal
     * pipe libuv uses for spawn-success detection has CLOEXEC and
     * relies on this — without it the post-pseudo-exec failure path
     * keeps writing into a pipe the parent treats as live, so the
     * parent thinks the spawn failed and never paints. */
    for (int i = 0; i < YOS_FD_MAX; i++) {
        int phfd = fork_thread_arg->parent_fd_map[i];
        if (phfd < 0) {
            child_ctx->fd_map[i] = -1;
            continue;
        }
        int flags = fcntl(phfd, F_GETFD);
        int dupcmd = (flags >= 0 && (flags & FD_CLOEXEC))
                       ? F_DUPFD_CLOEXEC : F_DUPFD;
        int chfd = fcntl(phfd, dupcmd, 0);
        child_ctx->fd_map[i] = (chfd >= 0) ? chfd : -1;
    }
    child_ctx->argc = fork_thread_arg->argc;
    child_ctx->argv = fork_thread_arg->argv;
    child_ctx->envc = fork_thread_arg->envc;
    child_ctx->envp = fork_thread_arg->envp;
    child_ctx->wasm_bytes = fork_thread_arg->wasm_bytes;
    child_ctx->wasm_bytes_size = fork_thread_arg->wasm_bytes_size;
    memcpy(child_ctx->cwd, fork_thread_arg->parent_cwd, sizeof(child_ctx->cwd));
    pthread_mutex_init(&child_ctx->mem_lock, NULL);

    rt->userdata = child_ctx;

    /* Parse and load module */
    IM3Module mod;
    M3Result res = m3_ParseModule(env, &mod, fork_thread_arg->wasm_bytes, fork_thread_arg->wasm_bytes_size);
    if (res) {
        free(child_ctx);
        m3_FreeRuntime(rt);
        m3_FreeEnvironment(env);
        free(fork_thread_arg->memory_snapshot);
        free(fork_thread_arg->wasm_globals);
        free(fork_thread_arg);
        return NULL;
    }

    res = m3_LoadModule(rt, mod);
    if (res) {
        free(child_ctx);
        m3_FreeRuntime(rt);
        m3_FreeEnvironment(env);
        free(fork_thread_arg->memory_snapshot);
        free(fork_thread_arg->wasm_globals);
        free(fork_thread_arg);
        return NULL;
    }

    child_ctx->module = mod;

    /* Link syscall functions to child runtime */
    extern void yos_link_imports(IM3Module module, struct yos_exec_ctx *ctx);
    yos_link_imports(mod, child_ctx);

    /* Grow memory to match parent BEFORE restoring snapshot */
    extern M3Result ResizeMemory(IM3Runtime, uint32_t);
    /* Must match the parent's memory size (set in main.c). 4096 pages
     * × 64 KiB = 256 MiB. If this is smaller than the parent, the
     * memcpy(snapshot) below fails and the child traps on its first
     * memory access. */
    ResizeMemory(rt, 4096);

    /* Restore memory from snapshot */
    uint32_t mem_size;
    uint8_t *mem = m3_GetMemory(rt, &mem_size, 0);
    if (mem && fork_thread_arg->memory_size <= mem_size) {
        memcpy(mem, fork_thread_arg->memory_snapshot, fork_thread_arg->memory_size);
        ydebug("child: restored %u bytes of memory\n", fork_thread_arg->memory_size);
    } else {
        ydebug("child: FAILED to restore memory! have %u, need %u\n", mem_size, fork_thread_arg->memory_size);
    }
    child_ctx->memory = mem;
    child_ctx->memory_size = mem_size;

    /* Restore globals (critical: includes stack pointer!) */
    for (uint32_t i = 0; i < fork_thread_arg->wasm_globals_count && i < mod->numGlobals; i++) {
        mod->globals[i].intValue = fork_thread_arg->wasm_globals[i];
    }
    ydebug("child: restored %u globals\n", fork_thread_arg->wasm_globals_count);

    free(fork_thread_arg->memory_snapshot);
    free(fork_thread_arg->wasm_globals);
    free(fork_thread_arg);

    /* Start rewind - when _start is called, it will rewind to fork point */
    call_asyncify(rt, "asyncify_start_rewind", child_ctx->asyncify_ptr);

    ydebug("child pid=%d starting rewind\n", child_ctx->proc->pid);

    /* Child exec loop - run _start, reload module if exec happens */
    for (;;) {
        IM3Function start_fn;
        res = m3_FindFunction(&start_fn, rt, "_start");
        if (res) {
            ydebug("child: _start not found: %s\n", res);
            child_ctx->proc->state = YOS_PROC_ZOMBIE;
            child_ctx->proc->exit_code = 127;
            child_ctx->proc->exited = 1;
            break;
        }

        res = m3_CallV(start_fn);

        /* Drive setjmp/longjmp asyncify round-trips until they settle.
         * The child runtime is its own pump — without this loop the
         * first setjmp unwinds out of _start and we'd mistake it for a
         * clean exit. yos_setjmp_pump is shared with main.c. */
        extern void yos_setjmp_pump(struct yos_exec_ctx *);
        while (child_ctx->setjmp_pending || child_ctx->longjmp_pending) {
            yos_setjmp_pump(child_ctx);
            if (child_ctx->pump_trap) break;
        }
        /* Surface a pump-internal trap as the loop's res. Without this,
         * a wasm crash inside the rewound _start would silently fall into
         * the "clean exit" branch. */
        if (child_ctx->pump_trap) {
            res = child_ctx->pump_trap;
            child_ctx->pump_trap = NULL;
        }

        /* Check if exec happened */
        if (!child_ctx->exec_pending) {
            if (res) {
                ydebug("child: trap: %s\n", res);
                child_ctx->proc->state = YOS_PROC_ZOMBIE;
                child_ctx->proc->exit_code = 139;
                child_ctx->proc->exited = 1;
            }
            break;
        }

        /* Handle exec - load new module */
        ydebug("child exec: loading %s\n", child_ctx->exec_path);

        m3_FreeRuntime(rt);
        m3_FreeEnvironment(env);

        child_ctx->argc = child_ctx->exec_argc;
        child_ctx->argv = child_ctx->exec_argv;
        /* Install the env that was passed to execve (if any). NULL
         * means "no override" — keep inherited envp so a forked child
         * that execs with envp=environ keeps the parent's environment. */
        if (child_ctx->exec_envp) {
            child_ctx->envc = child_ctx->exec_envc;
            child_ctx->envp = child_ctx->exec_envp;
        }

        env = m3_NewEnvironment();
        rt = m3_NewRuntime(env, 64 * 1024, NULL);
        if (!rt) {
            ydebug("child exec: failed to create runtime\n");
            child_ctx->proc->state = YOS_PROC_ZOMBIE;
            child_ctx->proc->exit_code = 127;
            child_ctx->proc->exited = 1;
            break;
        }

        FILE *f = fopen(child_ctx->exec_path, "rb");
        if (!f) {
            ydebug("child exec: cannot open %s\n", child_ctx->exec_path);
            m3_FreeRuntime(rt);
            m3_FreeEnvironment(env);
            rt = NULL;
            env = NULL;
            child_ctx->proc->state = YOS_PROC_ZOMBIE;
            child_ctx->proc->exit_code = 127;
            child_ctx->proc->exited = 1;
            break;
        }
        fseek(f, 0, SEEK_END);
        size_t wasm_size = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *wasm_bytes = malloc(wasm_size);
        if (!wasm_bytes || fread(wasm_bytes, 1, wasm_size, f) != wasm_size) {
            fclose(f);
            free(wasm_bytes);
            m3_FreeRuntime(rt);
            m3_FreeEnvironment(env);
            rt = NULL;
            env = NULL;
            child_ctx->proc->state = YOS_PROC_ZOMBIE;
            child_ctx->proc->exit_code = 127;
            child_ctx->proc->exited = 1;
            break;
        }
        fclose(f);

        res = m3_ParseModule(env, &mod, wasm_bytes, wasm_size);
        if (res) {
            ydebug("child exec: parse error: %s\n", res);
            free(wasm_bytes);
            m3_FreeRuntime(rt);
            m3_FreeEnvironment(env);
            rt = NULL;
            env = NULL;
            child_ctx->proc->state = YOS_PROC_ZOMBIE;
            child_ctx->proc->exit_code = 127;
            child_ctx->proc->exited = 1;
            break;
        }

        res = m3_LoadModule(rt, mod);
        if (res) {
            ydebug("child exec: load error: %s\n", res);
            free(wasm_bytes);
            m3_FreeRuntime(rt);
            m3_FreeEnvironment(env);
            rt = NULL;
            env = NULL;
            child_ctx->proc->state = YOS_PROC_ZOMBIE;
            child_ctx->proc->exit_code = 127;
            child_ctx->proc->exited = 1;
            break;
        }

        child_ctx->runtime = rt;
        child_ctx->module = mod;
        child_ctx->wasm_bytes = wasm_bytes;
        child_ctx->wasm_bytes_size = wasm_size;
        rt->userdata = child_ctx;
        yos_link_imports(mod, child_ctx);

        extern M3Result ResizeMemory(IM3Runtime, uint32_t);
        /* Match the parent's 4096 pages × 64 KiB = 256 MiB. nvim --embed
         * blows past 16 MiB just on Lua + module dictionaries. */
        ResizeMemory(rt, 4096);

        uint32_t mem_size;
        child_ctx->memory = m3_GetMemory(rt, &mem_size, 0);
        child_ctx->memory_size = mem_size;
        /* Heap starts at __heap_base — see comment in main.c:load_wasm_module
         * for why a hardcoded constant corrupts .data. Falls back to
         * 0x50000 if the wasm doesn't export __heap_base. */
        child_ctx->heap_end = 0x50000;
        {
            IM3Global g = m3_FindGlobal(mod, "__heap_base");
            if (g) {
                M3TaggedValue tv = { 0 };
                M3Result gr = m3_GetGlobal(g, &tv);
                if (!gr && tv.type == c_m3Type_i32) {
                    uint32_t hb = (tv.value.i32 + 15) & ~15u;
                    if (hb > child_ctx->heap_end) child_ctx->heap_end = hb;
                }
            }
        }
        /* Fresh module → fresh address space; let mmap2 re-anchor
         * itself at memory_size/2 by clearing the watermark. */
        child_ctx->mmap_top = 0;
        child_ctx->free_count = 0;

        uint32_t *thread_ptr = (uint32_t *)(child_ctx->memory + 0);
        *thread_ptr = 0x100;
        memset(child_ctx->memory + 0x100, 0, 256);

        child_ctx->exec_pending = 0;
        child_ctx->exec_argv = NULL;
        child_ctx->exec_argc = 0;
        child_ctx->exec_envp = NULL;
        child_ctx->exec_envc = 0;

        ydebug("child exec: loaded %s, argc=%d\n", child_ctx->exec_path, child_ctx->argc);
    }

    /* Child finished - mark as zombie */
    if (child_ctx->proc->state != YOS_PROC_ZOMBIE) {
        child_ctx->proc->state = YOS_PROC_ZOMBIE;
        child_ctx->proc->exit_code = 0;
        child_ctx->proc->exited = 1;
    }
    pthread_cond_broadcast(&child_ctx->proc->wait_cond);

    ydebug("child pid=%d exited\n", child_ctx->proc->pid);

    m3_FreeRuntime(rt);
    m3_FreeEnvironment(env);
    free(child_ctx);

    return NULL;
}

/* yos_fork_pump - called after WASM execution returns */
void yos_fork_pump(struct yos_exec_ctx *ctx)
{
    while (ctx->fork_pending) {
        ctx->fork_pending = 0;

        IM3Runtime wrt = (IM3Runtime)ctx->runtime;
        IM3Module mod = (IM3Module)ctx->module;

        /* Stop unwind - buffer now contains saved call stack */
        call_asyncify(wrt, "asyncify_stop_unwind", -1);
        ydebug("unwind stopped, copying state for child\n");

        /* Copy memory AFTER unwind */
        uint32_t mem_size;
        uint8_t *mem = m3_GetMemory(wrt, &mem_size, 0);
        uint8_t *mem_copy = malloc(mem_size);
        if (!mem_copy) {
            struct yos_proc *child = yos_proc_find(ctx->rt, ctx->fork_return);
            if (child) child->state = YOS_PROC_FREE;
            return;
        }
        memcpy(mem_copy, mem, mem_size);

        /* Save WASM globals */
        uint32_t num_globals = mod->numGlobals;
        int64_t *wasm_globals_copy = malloc(num_globals * sizeof(int64_t));
        if (!wasm_globals_copy) {
            free(mem_copy);
            struct yos_proc *child = yos_proc_find(ctx->rt, ctx->fork_return);
            if (child) child->state = YOS_PROC_FREE;
            return;
        }
        for (uint32_t i = 0; i < num_globals; i++) {
            wasm_globals_copy[i] = mod->globals[i].intValue;
        }

        /* Find child process */
        struct yos_proc *child_proc = yos_proc_find(ctx->rt, ctx->fork_return);
        if (!child_proc) {
            free(mem_copy);
            free(wasm_globals_copy);
            return;
        }

        /* Prepare thread argument */
        fork_thread_arg_t *fork_thread_arg = malloc(sizeof(fork_thread_arg_t));
        if (!fork_thread_arg) {
            free(mem_copy);
            free(wasm_globals_copy);
            child_proc->state = YOS_PROC_FREE;
            return;
        }
        fork_thread_arg->rt = ctx->rt;
        fork_thread_arg->proc = child_proc;
        fork_thread_arg->memory_snapshot = mem_copy;
        fork_thread_arg->memory_size = mem_size;
        fork_thread_arg->asyncify_ptr    = ctx->asyncify_ptr;
        /* TODO(setjmp-refactor): forward ctx->sj_slots[] to the child. */
        fork_thread_arg->sj_discard_ptr  = ctx->sj_discard_ptr;
        fork_thread_arg->wasm_globals = wasm_globals_copy;
        fork_thread_arg->wasm_globals_count = num_globals;
        fork_thread_arg->heap_end = ctx->heap_end;
        fork_thread_arg->argc = ctx->argc;
        fork_thread_arg->argv = ctx->argv;
        fork_thread_arg->envc = ctx->envc;
        fork_thread_arg->envp = ctx->envp;
        fork_thread_arg->wasm_bytes = ctx->wasm_bytes;
        fork_thread_arg->wasm_bytes_size = ctx->wasm_bytes_size;
        memcpy(fork_thread_arg->parent_fd_map, ctx->fd_map,
               sizeof(ctx->fd_map));
        memcpy(fork_thread_arg->parent_cwd, ctx->cwd,
               sizeof(fork_thread_arg->parent_cwd));

        /* Spawn child thread detached so the parent resumes concurrently.
         * Child lifetime is tracked via yos_proc state (RUNNING/ZOMBIE);
         * waitpid reaps via the process table, not via pthread_join. */
        child_proc->state = YOS_PROC_RUNNING;
        pthread_t t;
        int r = pthread_create(&t, NULL, fork_thread_func, fork_thread_arg);
        if (r != 0) {
            free(mem_copy);
            free(wasm_globals_copy);
            free(fork_thread_arg);
            child_proc->state = YOS_PROC_FREE;
            return;
        }
        pthread_detach(t);

        ydebug("forked child pid=%d, resuming parent\n", child_proc->pid);

        /* Resume parent - start rewind */
        call_asyncify(wrt, "asyncify_start_rewind", ctx->asyncify_ptr);

        /* Call _start again - this will rewind to fork point */
        IM3Function start;
        m3_FindFunction(&start, wrt, "_start");
        m3_CallV(start);
    }
}

/* Wait options from linux/wait.h */
#define WNOHANG    0x00000001

/* Find a zombie child to reap. Returns NULL if none found.
 * pid: -1 = any child, >0 = specific pid */
static struct yos_proc *find_zombie_child(struct yos_runtime *rt, int32_t ppid, int32_t pid)
{
    for (int i = 0; i < YOS_MAX_PROCS; i++) {
        struct yos_proc *p = &rt->procs[i];
        if (p->state == YOS_PROC_ZOMBIE && p->ppid == ppid) {
            if (pid == -1 || p->pid == pid)
                return p;
        }
    }
    return NULL;
}

/* Check if there are any children (zombie or running) */
static int has_children(struct yos_runtime *rt, int32_t ppid, int32_t pid)
{
    for (int i = 0; i < YOS_MAX_PROCS; i++) {
        struct yos_proc *p = &rt->procs[i];
        if (p->state != YOS_PROC_FREE && p->ppid == ppid) {
            if (pid == -1 || p->pid == pid)
                return 1;
        }
    }
    return 0;
}

int32_t yos_waitpid(struct yos_exec_ctx *ctx, int32_t pid, uint32_t stat_addr, int32_t options)
{
    ydebug("waitpid(%d, 0x%x, %d)\n", pid, stat_addr, options);

    int32_t my_pid = ctx->proc->pid;
    struct yos_runtime *rt = ctx->rt;

    pthread_mutex_lock(&rt->proc_lock);

    /* Check if child exists */
    if (!has_children(rt, my_pid, pid)) {
        pthread_mutex_unlock(&rt->proc_lock);
        return -ECHILD;
    }

    /* Find zombie child */
    struct yos_proc *child = find_zombie_child(rt, my_pid, pid);

    if (!child && (options & WNOHANG)) {
        pthread_mutex_unlock(&rt->proc_lock);
        return 0;  /* No zombie, non-blocking */
    }

    /* Wait for child to become zombie */
    while (!child) {
        pthread_mutex_unlock(&rt->proc_lock);
        /* TODO: proper blocking wait on child's wait_cond */
        usleep(1000);
        pthread_mutex_lock(&rt->proc_lock);
        child = find_zombie_child(rt, my_pid, pid);
    }

    /* Reap the zombie */
    int32_t child_pid = child->pid;
    int32_t exit_code = child->exit_code;
    child->state = YOS_PROC_FREE;

    pthread_mutex_unlock(&rt->proc_lock);

    /* Write status: (exit_code << 8) like Linux */
    if (stat_addr && stat_addr < ctx->memory_size - 4) {
        *(int32_t *)(ctx->memory + stat_addr) = (exit_code & 0xff) << 8;
    }

    ydebug("waitpid = %d (exit_code=%d)\n", child_pid, exit_code);
    return child_pid;
}

/* Check WASM magic bytes */
static int check_wasm_magic(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t magic[4];
    int ok = (fread(magic, 1, 4, f) == 4 &&
              magic[0] == 0x00 && magic[1] == 0x61 &&
              magic[2] == 0x73 && magic[3] == 0x6d);
    fclose(f);
    return ok;
}

/* Parse shebang line. Returns 0 on success, -1 if not a shebang.
 * interp and arg are output buffers (PATH_MAX size). */
static int parse_shebang(const char *path, char *interp, char *arg)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char line[256];
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return -1;
    }
    fclose(f);

    /* Must start with #! */
    if (line[0] != '#' || line[1] != '!') return -1;

    /* Skip #! and leading whitespace */
    char *p = line + 2;
    while (*p == ' ' || *p == '\t') p++;

    /* Extract interpreter path */
    char *start = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;

    size_t len = p - start;
    if (len == 0 || len >= PATH_MAX) return -1;
    memcpy(interp, start, len);
    interp[len] = '\0';

    /* Skip whitespace before optional arg */
    while (*p == ' ' || *p == '\t') p++;

    /* Extract optional argument (single arg only, like kernel) */
    arg[0] = '\0';
    if (*p && *p != '\n' && *p != '\r') {
        start = p;
        while (*p && *p != '\n' && *p != '\r') p++;
        /* Trim trailing whitespace */
        while (p > start && (p[-1] == ' ' || p[-1] == '\t')) p--;
        len = p - start;
        if (len > 0 && len < PATH_MAX) {
            memcpy(arg, start, len);
            arg[len] = '\0';
        }
    }

    return 0;
}

/*
 * Host executable execution from inside a yos guest is gated by
 * YOS_ALLOW_HOST_EXEC. Default = undefined = host execs blocked: only wasm
 * binaries (or scripts with a wasm interpreter via shebang) load. When
 * defined at compile time (e.g. -DYOS_ALLOW_HOST_EXEC=1), execve falls
 * through to a real host execv() that replaces this yos process with the
 * target binary — matching POSIX execve semantics, but breaking the wasm
 * sandbox. Off by default; only flip on for a yos build that explicitly
 * acts as a wasm-↔-native bridge.
 */
static int32_t host_execve_fallback(const char *fn, struct yos_exec_ctx *ctx,
                                    uint32_t argv_ptr)
{
#ifdef YOS_ALLOW_HOST_EXEC
    /* Marshal wasm argv into host argv (strdup so the originals can be
     * freed when the wasm runtime tears down). */
    uint32_t *wasm_argv = (uint32_t *)(ctx->memory + argv_ptr);
    int argc = 0;
    while (wasm_argv[argc] != 0 && argc < 1024) argc++;
    char **host_argv = malloc((argc + 1) * sizeof(char *));
    if (!host_argv) return -ENOMEM;
    for (int i = 0; i < argc; ++i)
        host_argv[i] = strdup((const char *)(ctx->memory + wasm_argv[i]));
    host_argv[argc] = NULL;

    ydebug("execve: YOS_ALLOW_HOST_EXEC: replacing yos with host %s\n", fn);
    execv(fn, host_argv);
    /* execv returned → it failed; report errno to the wasm caller. */
    int err = errno;
    for (int i = 0; i < argc; ++i) free(host_argv[i]);
    free(host_argv);
    return -err;
#else
    (void)fn; (void)ctx; (void)argv_ptr;
    return -ENOEXEC;
#endif
}

int32_t yos_execve(struct yos_exec_ctx *ctx, uint32_t filename, uint32_t argv_ptr, uint32_t envp)
{
    const char *fn = (const char *)(ctx->memory + filename);
    ydebug("execve(%s, ...)\n", fn);

    /* Check file exists and is readable */
    if (access(fn, R_OK) != 0) {
        ydebug("execve: %s: %s\n", fn, strerror(errno));
        return -errno;
    }

    char exec_path[PATH_MAX];
    char shebang_interp[PATH_MAX];
    char shebang_arg[PATH_MAX];
    int is_script = 0;
    int extra_args = 0;  /* args to prepend from shebang */

    /* Check file format */
    if (check_wasm_magic(fn)) {
        /* Direct WASM execution */
        strcpy(exec_path, fn);
    } else if (parse_shebang(fn, shebang_interp, shebang_arg) == 0) {
        /* Script with shebang - interpreter must be WASM */
        ydebug("execve: shebang interp=%s arg=%s\n", shebang_interp, shebang_arg);

        if (!check_wasm_magic(shebang_interp)) {
            ydebug("execve: interpreter %s is not WASM\n", shebang_interp);
            return host_execve_fallback(fn, ctx, argv_ptr);
        }
        strcpy(exec_path, shebang_interp);
        is_script = 1;
        extra_args = shebang_arg[0] ? 2 : 1;  /* interp [arg] script */
    } else {
        ydebug("execve: %s: unknown format\n", fn);
        return host_execve_fallback(fn, ctx, argv_ptr);
    }

    /* Count original argv entries */
    uint32_t *wasm_argv = (uint32_t *)(ctx->memory + argv_ptr);
    int orig_argc = 0;
    while (wasm_argv[orig_argc] != 0) {
        orig_argc++;
        if (orig_argc > 1024) return -E2BIG;
    }

    /* For scripts: argv[0] is replaced, script path inserted after interp */
    int new_argc = is_script ? (extra_args + orig_argc) : orig_argc;

    /* Allocate host argv */
    char **host_argv = malloc((new_argc + 1) * sizeof(char *));
    if (!host_argv) return -ENOMEM;

    int ai = 0;

    if (is_script) {
        /* argv[0] = interpreter basename or original argv[0] behavior */
        host_argv[ai++] = strdup(shebang_interp);
        if (shebang_arg[0]) {
            host_argv[ai++] = strdup(shebang_arg);
        }
        /* Insert script path */
        host_argv[ai++] = strdup(fn);
        /* Copy remaining original args (skip argv[0]) */
        for (int i = 1; i < orig_argc; i++) {
            const char *src = (const char *)(ctx->memory + wasm_argv[i]);
            host_argv[ai++] = strdup(src);
        }
    } else {
        /* Direct exec: copy all original args */
        for (int i = 0; i < orig_argc; i++) {
            const char *src = (const char *)(ctx->memory + wasm_argv[i]);
            host_argv[ai++] = strdup(src);
        }
    }
    host_argv[ai] = NULL;

    /* Debug */
    for (int i = 0; i < ai; i++) {
        ydebug("execve: argv[%d] = %s\n", i, host_argv[i]);
    }

    /* Capture envp from wasm-side caller. NULL/0 means "no env override" —
     * fall back to ctx->envp at exec-load time. POSIX execve takes a
     * full env replacement (it does NOT merge), so we copy whatever
     * the caller passed verbatim. */
    char **host_envp = NULL;
    int env_count = 0;
    if (envp != 0) {
        uint32_t *wasm_envp = (uint32_t *)(ctx->memory + envp);
        while (wasm_envp[env_count] != 0) {
            env_count++;
            if (env_count > 4096) return -E2BIG;
        }
        host_envp = malloc((env_count + 1) * sizeof(char *));
        if (!host_envp) {
            for (int i = 0; i < ai; i++) free(host_argv[i]);
            free(host_argv);
            return -ENOMEM;
        }
        for (int i = 0; i < env_count; i++) {
            const char *src = (const char *)(ctx->memory + wasm_envp[i]);
            host_envp[i] = strdup(src);
        }
        host_envp[env_count] = NULL;
    }

    /* Store exec info */
    strcpy(ctx->exec_path, exec_path);
    ctx->exec_pending = 1;
    ctx->exec_argc = ai;
    ctx->exec_argv = host_argv;
    ctx->exec_envc = env_count;
    ctx->exec_envp = host_envp;

    ydebug("execve: exec_pending set, path=%s argc=%d\n", exec_path, ai);

    /* POSIX exec closes every fd whose FD_CLOEXEC flag is set. Real
     * host-execve does this implicitly; yos's pseudo-exec leaves the
     * shared host fd table intact, so we have to walk fd_map and close
     * the marked entries ourselves. Without this, libuv's spawn signal
     * pipe (O_CLOEXEC) stays open after exec, the post-execvp failure
     * path in the child writes errno into it, and the parent reads
     * that errno and treats the spawn as failed. */
    for (int i = 0; i < YOS_FD_MAX; i++) {
        int hfd = ctx->fd_map[i];
        if (hfd < 0) continue;
        int flags = fcntl(hfd, F_GETFD);
        if (flags < 0) continue;
        if (flags & FD_CLOEXEC) {
            close(hfd);
            ctx->fd_map[i] = -1;
        }
    }

    /* Return 0 (success) - syscall handler will trap to stop WASM execution */
    return 0;
}

/* execv(path, argv) — POSIX wrapper around execve with the caller's
 * existing env. We pass envp=0 to yos_execve which means "no env
 * replacement" (the reloaded module inherits ctx->envp). */
int32_t yos_execv(struct yos_exec_ctx *ctx, uint32_t path, uint32_t argv_ptr)
{
    return yos_execve(ctx, path, argv_ptr, 0);
}

/* execvp(file, argv) — searches PATH if `file` has no slash. We don't
 * implement PATH search yet; if the caller passes an absolute or
 * relative path with a slash (which is what libuv / nvim does for the
 * self-respawn path), it works. Bare file names return -ENOENT. */
int32_t yos_execvp(struct yos_exec_ctx *ctx, uint32_t file, uint32_t argv_ptr)
{
    const char *fn = (const char *)(ctx->memory + file);
    if (!strchr(fn, '/')) {
        ydebug("execvp(%s): PATH search not implemented; ENOENT\n", fn);
        return -ENOENT;
    }
    return yos_execve(ctx, file, argv_ptr, 0);
}

/* execvpe(file, argv, envp) — same as execvp but with explicit envp. */
int32_t yos_execvpe(struct yos_exec_ctx *ctx, uint32_t file,
                    uint32_t argv_ptr, uint32_t envp)
{
    const char *fn = (const char *)(ctx->memory + file);
    if (!strchr(fn, '/')) {
        ydebug("execvpe(%s): PATH search not implemented; ENOENT\n", fn);
        return -ENOENT;
    }
    return yos_execve(ctx, file, argv_ptr, envp);
}

/* deliver_to_proc: send `sig` to one guest proc via pthread_kill on its
 * thread, returning kill(2)-style status (0 on success or -errno).
 * sig==0 is the existence probe — no signal sent. */
static int deliver_to_proc(struct yos_proc *p, int sig)
{
    if (p->state == YOS_PROC_FREE || p->state == YOS_PROC_ZOMBIE)
        return -ESRCH;
    if (sig == 0) return 0;
    if (!p->thread) return -ESRCH;
    int rc = pthread_kill(p->thread, sig);
    return rc == 0 ? 0 : -rc;
}

int32_t yos_kill(struct yos_exec_ctx *ctx, int32_t pid, int32_t sig)
{
    ydebug("kill(%d, %d)\n", pid, sig);
    struct yos_runtime *rt = ctx->rt;
    int32_t my_pgid = ctx->proc ? ctx->proc->pgid : 0;

    /* pid > 0: deliver to that guest proc. */
    if (pid > 0) {
        struct yos_proc *t = yos_proc_find(rt, pid);
        if (!t) return -ESRCH;
        return deliver_to_proc(t, sig);
    }

    /* pid == 0: deliver to every proc in caller's pgrp. */
    /* pid == -1: deliver to every proc except init (and the caller). */
    /* pid <  -1: deliver to every proc whose pgid == -pid. */
    int32_t target_pgid = (pid == 0) ? my_pgid : (pid < -1 ? -pid : 0);
    int found = 0, last_err = 0;
    pthread_mutex_lock(&rt->proc_lock);
    for (int i = 0; i < YOS_MAX_PROCS; i++) {
        struct yos_proc *p = &rt->procs[i];
        if (p->state == YOS_PROC_FREE || p->state == YOS_PROC_ZOMBIE) continue;
        int match = 0;
        if (pid == 0)         match = (p->pgid == my_pgid);
        else if (pid == -1)   match = (p->pid != 1);  /* skip init */
        else                  match = (p->pgid == target_pgid);
        if (!match) continue;
        found++;
        int rc = (sig == 0) ? 0 : pthread_kill(p->thread, sig);
        if (rc != 0) last_err = -rc;
    }
    pthread_mutex_unlock(&rt->proc_lock);
    if (!found) return -ESRCH;
    return last_err;
}

int32_t yos_wait4(struct yos_exec_ctx *ctx, int32_t pid, uint32_t stat_addr, int32_t options, uint32_t ru)
{
    ydebug("wait4(%d, 0x%x, %d, 0x%x)\n", pid, stat_addr, options, ru);
    (void)ru; /* TODO: rusage conversion */

    int32_t my_pid = ctx->proc->pid;
    struct yos_runtime *rt = ctx->rt;

    pthread_mutex_lock(&rt->proc_lock);

    /* Check if child exists */
    if (!has_children(rt, my_pid, pid)) {
        pthread_mutex_unlock(&rt->proc_lock);
        ydebug("wait4 = -ECHILD\n");
        return -ECHILD;
    }

    /* Find zombie child */
    struct yos_proc *child = find_zombie_child(rt, my_pid, pid);

    if (!child && (options & WNOHANG)) {
        pthread_mutex_unlock(&rt->proc_lock);
        ydebug("wait4 = 0 (WNOHANG)\n");
        return 0;
    }

    /* Wait for child to become zombie */
    while (!child) {
        pthread_mutex_unlock(&rt->proc_lock);
        usleep(1000);
        pthread_mutex_lock(&rt->proc_lock);
        child = find_zombie_child(rt, my_pid, pid);
    }

    /* Reap the zombie */
    int32_t child_pid = child->pid;
    int32_t exit_code = child->exit_code;
    child->state = YOS_PROC_FREE;

    pthread_mutex_unlock(&rt->proc_lock);

    /* Write status: (exit_code << 8) like Linux */
    if (stat_addr && stat_addr < ctx->memory_size - 4) {
        *(int32_t *)(ctx->memory + stat_addr) = (exit_code & 0xff) << 8;
    }

    ydebug("wait4 = %d (exit_code=%d)\n", child_pid, exit_code);
    return child_pid;
}

/* clone() — the SINGLE process-creation primitive. Both fork and
 * pthread_create eventually route here. Branches on CLONE_VM:
 *   CLONE_VM set   → thread path (sibling wasm3 runtime, shared memory)
 *   CLONE_VM clear → fork path (asyncify snapshot + restore in fresh runtime)
 *
 * i386 SYS_clone arg order: (flags, child_stack, parent_tid, child_tid, tls).
 *
 * fn / arg are not separate syscall args on i386 — musl's __clone
 * pushes them onto the supplied stack before the syscall. We read them
 * back from wasm linear memory at child_stack-8 (fn) and child_stack-4
 * (arg), matching the standard x86 ABI musl already implements.
 *
 * The wasm32 musl __clone shim (see src/musl/arch/wasm32/thread/...)
 * is responsible for doing those pushes; without that, fn/arg arrive
 * as garbage and we must reject. */
int32_t yos_proc_clone(struct yos_exec_ctx *ctx,
                       uint32_t flags,
                       uint32_t child_stack,
                       uint32_t ptid_addr,
                       uint32_t ctid_addr,
                       uint32_t tls)
{
    /* CLONE_* constants come from <sched.h> via <pthread.h> in types.h. */
    ydebug("clone(flags=0x%x, stack=0x%x, ptid=0x%x, ctid=0x%x, tls=0x%x)\n",
           flags, child_stack, ptid_addr, ctid_addr, tls);

    /* Fork path: no shared memory. Delegate to the existing fork
     * machinery (asyncify-based copy). vfork is fork + parent-blocks
     * which we already have separately. */
    if (!(flags & CLONE_VM)) {
        /* Fall back to fork() for now. CLONE_PARENT/CLONE_PTRACE/etc.
         * are accepted-and-ignored here; the proc table fields land
         * the same way as a plain fork. */
        return yos_fork(ctx);
    }

    /* Thread path. CLONE_VM without CLONE_THREAD is unusual (would mean
     * "share memory but be a separate process"); we treat it as a
     * thread anyway since the substrate (sibling wasm3 runtime) doesn't
     * model that distinction. */
    if (!ctx->proc || !ctx->rt) return -EINVAL;

    /* Read fn and arg from where musl's wasm32 __clone shim wrote them:
     * the i386 ABI says they live just below child_stack (which is the
     * top-of-stack passed in). Bounds-check before touching memory. */
    if (child_stack < 8 || child_stack > ctx->memory_size)
        return -EFAULT;
    uint32_t *sp = (uint32_t *)(ctx->memory + child_stack - 8);
    uint32_t fn  = sp[0];
    uint32_t arg = sp[1];
    if (fn == 0) {
        /* Either the shim hasn't pushed yet or the caller is doing
         * raw clone() with an undocumented stack convention. */
        ydebug("clone: fn at 0x%x is 0 — wasm32 __clone shim not pushing fn/arg?\n",
               child_stack - 8);
        return -EINVAL;
    }

    /* Allocate the new yos_proc. Thread variant: tgid inherited from
     * caller's tgid so getpid() agrees across threads of one task,
     * pid is the unique tid. */
    struct yos_proc *child = yos_proc_alloc(ctx->rt, ctx->proc->pid);
    if (!child) return -EAGAIN;
    if (flags & CLONE_THREAD) {
        child->tgid = ctx->proc->tgid;
    }
    child->pgid = ctx->proc->pgid;
    child->sid  = ctx->proc->sid;
    if (flags & CLONE_CHILD_CLEARTID) child->tid_address = ctid_addr;

    /* Spawn through the same internal substrate the L1 import uses. */
    extern int yos_clone_thread(void *h, uint32_t fn_idx, uint32_t arg,
                                uint32_t ctid_addr, uint32_t tls,
                                uint8_t *memory_base, uint32_t *out_tid);
    uint32_t spawned_tid = 0;
    int rc = yos_clone_thread(ctx->rt->pthread_host,
                              fn, arg,
                              (flags & CLONE_CHILD_CLEARTID) ? ctid_addr : 0,
                              (flags & CLONE_SETTLS) ? tls : 0,
                              ctx->memory,
                              &spawned_tid);
    if (rc != 0) {
        child->state = YOS_PROC_FREE;
        return rc;
    }

    /* CLONE_PARENT_SETTID: parent gets the new tid via *ptid_addr. */
    if ((flags & CLONE_PARENT_SETTID) && ptid_addr) {
        if (ptid_addr + 4 <= ctx->memory_size)
            *(uint32_t *)(ctx->memory + ptid_addr) = (uint32_t)child->pid;
    }
    /* CLONE_CHILD_SETTID: kernel writes tid into the child-side ctid
     * address up front (the matching CLEARTID + futex-wake on exit
     * brings it back to 0). musl's pthread_create relies on the
     * write happening before clone() returns, so anyone futex_wait'ing
     * on the address sees the non-zero tid value and parks correctly. */
    #ifndef CLONE_CHILD_SETTID
    #  define CLONE_CHILD_SETTID 0x01000000
    #endif
    if ((flags & CLONE_CHILD_SETTID) && ctid_addr) {
        if (ctid_addr + 4 <= ctx->memory_size) {
            *(uint32_t *)(ctx->memory + ctid_addr) = (uint32_t)child->pid;
            ydebug("clone: CHILD_SETTID wrote %d to ctid=0x%x\n",
                   child->pid, ctid_addr);
        }
    }

    ydebug("clone -> pid=%d (host slot=%u)\n", child->pid, spawned_tid);
    (void)spawned_tid;
    return child->pid;
}

/* ============================================================================
 * Vfork Implementation
 *
 * vfork is like fork but:
 * 1. Parent blocks until child calls exec or _exit
 * 2. Child shares parent's memory (in real vfork - we copy anyway in wasm)
 *
 * Flow:
 * 1. Create child like fork
 * 2. Set child->vfork_parent_pid = parent pid
 * 3. Parent blocks on parent->vfork_cond
 * 4. Child runs, calls exec or _exit
 * 5. exec/_exit signals parent->vfork_cond
 * 6. Parent unblocks, continues
 * ============================================================================ */

int32_t yos_vfork(struct yos_exec_ctx *ctx)
{
    if (!ctx->proc || !ctx->rt) {
        ydebug("vfork: invalid context\n");
        return -EINVAL;
    }

    IM3Runtime wrt = (IM3Runtime)ctx->runtime;
    IM3Module mod = (IM3Module)ctx->module;

    if (!wrt || !mod) {
        ydebug("vfork: no wasm runtime\n");
        return -ENOMEM;
    }

    int state = get_asyncify_state(wrt);

    /* If asyncify not available, vfork cannot work */
    if (state < 0) {
        ydebug("vfork: asyncify not available\n");
        return -ENOSYS;
    }

    /* REWIND path: returning from vfork after rewind */
    if (state == ASYNCIFY_REWINDING) {
        call_asyncify(wrt, "asyncify_stop_rewind", -1);
        ydebug("vfork rewind complete, returning %d\n", ctx->fork_return);
        return ctx->fork_return;
    }

    /* FIRST CALL path: trigger unwind */
    ydebug("vfork called by pid=%d\n", ctx->proc->pid);

    /* Get memory for asyncify buffer */
    uint32_t mem_size;
    uint8_t *mem = m3_GetMemory(wrt, &mem_size, 0);
    if (!mem || mem_size == 0) {
        return -ENOMEM;
    }

    /* Allocate asyncify buffer */
    if (ctx->asyncify_ptr == 0) {
        ctx->asyncify_ptr = mem_size - ASYNCIFY_BUF_SIZE;
    }

    /* Reset asyncify buffer header */
    uint32_t *buf = (uint32_t *)(mem + ctx->asyncify_ptr);
    buf[0] = ctx->asyncify_ptr + 8;
    buf[1] = ctx->asyncify_ptr + ASYNCIFY_BUF_SIZE;

    /* Allocate child process slot */
    struct yos_proc *child_proc = yos_proc_alloc(ctx->rt, ctx->proc->pid);
    if (!child_proc) {
        return -EAGAIN;
    }
    child_proc->pgid = ctx->proc->pgid;
    child_proc->sid = ctx->proc->sid;

    /* vfork specific: child stores parent pid, parent will block */
    child_proc->vfork_parent_pid = ctx->proc->pid;
    ctx->proc->vfork_child_done = 0;

    /* Store child pid as return value for parent */
    ctx->fork_return = child_proc->pid;
    ctx->fork_pending = 1;

    ydebug("vfork: starting unwind, child pid=%d, parent will block\n", child_proc->pid);
    call_asyncify(wrt, "asyncify_start_unwind", ctx->asyncify_ptr);

    return child_proc->pid;
}

/* vfork_pump - like fork_pump but parent blocks until child signals */
void yos_vfork_pump(struct yos_exec_ctx *ctx)
{
    while (ctx->fork_pending) {
        ctx->fork_pending = 0;

        IM3Runtime wrt = (IM3Runtime)ctx->runtime;
        IM3Module mod = (IM3Module)ctx->module;

        call_asyncify(wrt, "asyncify_stop_unwind", -1);

        /* Copy memory */
        uint32_t mem_size;
        uint8_t *mem = m3_GetMemory(wrt, &mem_size, 0);
        uint8_t *mem_copy = malloc(mem_size);
        if (!mem_copy) {
            struct yos_proc *child = yos_proc_find(ctx->rt, ctx->fork_return);
            if (child) child->state = YOS_PROC_FREE;
            return;
        }
        memcpy(mem_copy, mem, mem_size);

        /* Save globals */
        uint32_t num_globals = mod->numGlobals;
        int64_t *wasm_globals_copy = malloc(num_globals * sizeof(int64_t));
        if (!wasm_globals_copy) {
            free(mem_copy);
            struct yos_proc *child = yos_proc_find(ctx->rt, ctx->fork_return);
            if (child) child->state = YOS_PROC_FREE;
            return;
        }
        for (uint32_t i = 0; i < num_globals; i++) {
            wasm_globals_copy[i] = mod->globals[i].intValue;
        }

        struct yos_proc *child_proc = yos_proc_find(ctx->rt, ctx->fork_return);
        if (!child_proc) {
            free(mem_copy);
            free(wasm_globals_copy);
            return;
        }

        /* Prepare thread argument */
        fork_thread_arg_t *fork_thread_arg = malloc(sizeof(fork_thread_arg_t));
        if (!fork_thread_arg) {
            free(mem_copy);
            free(wasm_globals_copy);
            child_proc->state = YOS_PROC_FREE;
            return;
        }
        fork_thread_arg->rt = ctx->rt;
        fork_thread_arg->proc = child_proc;
        fork_thread_arg->memory_snapshot = mem_copy;
        fork_thread_arg->memory_size = mem_size;
        fork_thread_arg->asyncify_ptr    = ctx->asyncify_ptr;
        /* TODO(setjmp-refactor): forward ctx->sj_slots[] to the child. */
        fork_thread_arg->sj_discard_ptr  = ctx->sj_discard_ptr;
        fork_thread_arg->wasm_globals = wasm_globals_copy;
        fork_thread_arg->wasm_globals_count = num_globals;
        fork_thread_arg->heap_end = ctx->heap_end;
        fork_thread_arg->argc = ctx->argc;
        fork_thread_arg->argv = ctx->argv;
        fork_thread_arg->envc = ctx->envc;
        fork_thread_arg->envp = ctx->envp;
        fork_thread_arg->wasm_bytes = ctx->wasm_bytes;
        fork_thread_arg->wasm_bytes_size = ctx->wasm_bytes_size;
        memcpy(fork_thread_arg->parent_fd_map, ctx->fd_map,
               sizeof(ctx->fd_map));
        memcpy(fork_thread_arg->parent_cwd, ctx->cwd,
               sizeof(fork_thread_arg->parent_cwd));

        /* Spawn child thread */
        child_proc->state = YOS_PROC_RUNNING;
        pthread_t t;
        int r = pthread_create(&t, NULL, fork_thread_func, fork_thread_arg);
        if (r != 0) {
            free(mem_copy);
            free(wasm_globals_copy);
            free(fork_thread_arg);
            child_proc->state = YOS_PROC_FREE;
            return;
        }

        /* vfork: parent blocks until child calls exec or _exit */
        ydebug("vfork: parent pid=%d blocking until child done\n", ctx->proc->pid);
        pthread_mutex_lock(&ctx->proc->lock);
        while (!ctx->proc->vfork_child_done) {
            pthread_cond_wait(&ctx->proc->vfork_cond, &ctx->proc->lock);
        }
        ctx->proc->vfork_child_done = 0;
        pthread_mutex_unlock(&ctx->proc->lock);
        ydebug("vfork: parent pid=%d unblocked\n", ctx->proc->pid);

        pthread_join(t, NULL);

        /* Resume parent */
        call_asyncify(wrt, "asyncify_start_rewind", ctx->asyncify_ptr);

        IM3Function start;
        m3_FindFunction(&start, wrt, "_start");
        m3_CallV(start);
    }
}

int32_t yos_tkill(struct yos_exec_ctx *ctx, int32_t tid, int32_t sig)
{
    ydebug("tkill(%d, %d)\n", tid, sig);
    /* yos models one thread per guest process — guest tid == guest pid. */
    if (tid <= 0) return -ESRCH;
    struct yos_proc *t = yos_proc_find(ctx->rt, tid);
    if (!t) return -ESRCH;
    return deliver_to_proc(t, sig);
}

int32_t yos_proc_set_thread_area(struct yos_exec_ctx *ctx)
{
    (void)ctx;
    ydebug("set_thread_area() = 0 (stub)\n");
    return 0; /* single-threaded wasm - pretend success */
}

int32_t yos_proc_get_thread_area(struct yos_exec_ctx *ctx)
{
    (void)ctx;
    ydebug("get_thread_area() = 0 (stub)\n");
    return 0; /* single-threaded wasm - pretend success */
}

int32_t yos_proc_exit_group(struct yos_exec_ctx *ctx, int32_t error_code)
{
    ydebug("exit_group(%d)\n", error_code);
    return yos_exit(ctx, error_code);
}

int32_t yos_proc_set_tid_address(struct yos_exec_ctx *ctx, uint32_t tidptr)
{
    ydebug("set_tid_address(0x%x)\n", tidptr);
    /* Late-bound CLONE_CHILD_CLEARTID: kernel will write 0 here and
     * futex-wake on thread exit. yos_exit consults this field. */
    if (ctx->proc) ctx->proc->tid_address = tidptr;
    return ctx->proc ? ctx->proc->pid : 1;
}

int32_t yos_tgkill(struct yos_exec_ctx *ctx, int32_t tgid, int32_t tid, int32_t sig)
{
    ydebug("tgkill(%d, %d, %d)\n", tgid, tid, sig);
    if (tid <= 0) return -ESRCH;
    struct yos_proc *t = yos_proc_find(ctx->rt, tid);
    if (!t) return -ESRCH;
    /* tgid > 0 sanity-check: must match the proc's tgid (= its pid in
     * our one-thread-per-proc model). */
    if (tgid > 0 && tgid != t->pid) return -ESRCH;
    return deliver_to_proc(t, sig);
}

int32_t yos_waitid(struct yos_exec_ctx *ctx, int32_t which, int32_t pid, uint32_t infop, int32_t options, uint32_t ru)
{
    (void)ru; /* TODO: rusage */
    ydebug("waitid(%d, %d, 0x%x, %d)\n", which, pid, infop, options);
    siginfo_t info;
    int ret = waitid((idtype_t)which, (id_t)pid, &info, options);
    if (ret < 0) {
        ydebug("waitid = %d (errno=%d)\n", -errno, errno);
        return -errno;
    }
    /* Copy siginfo to wasm memory - simplified, just copy pid and status */
    if (infop && infop < ctx->memory_size - 128) {
        int32_t *si = (int32_t *)(ctx->memory + infop);
        si[0] = info.si_signo;
        si[1] = info.si_errno;
        si[2] = info.si_code;
        si[3] = info.si_pid;
        si[4] = info.si_uid;
        si[5] = info.si_status;
    }
    ydebug("waitid = 0\n");
    return 0;
}

/* Identity getters: return guest values from the per-runtime proc
 * table. Forwarding to host getpid/etc. would leak the host TID
 * (a 6-digit number) into the guest namespace. */
int32_t yos_getpid(struct yos_exec_ctx *ctx)
{
    /* Linux: getpid() returns the thread-group id, not the per-thread
     * tid. For non-thread procs (init, fork, vfork) tgid == pid so
     * this is the same number; for clone(CLONE_THREAD) children, tgid
     * is the parent's tgid and pid is unique. */
    return ctx->proc ? ctx->proc->tgid : 1;
}

int32_t yos_getppid(struct yos_exec_ctx *ctx)
{
    return ctx->proc ? ctx->proc->ppid : 0;
}

int32_t yos_getsid(struct yos_exec_ctx *ctx, int32_t pid)
{
    if (pid == 0) return ctx->proc ? ctx->proc->sid : 1;
    struct yos_proc *p = yos_proc_find(ctx->rt, pid);
    return p ? p->sid : -ESRCH;
}

int32_t yos_getpgid(struct yos_exec_ctx *ctx, int32_t pid)
{
    if (pid == 0) return ctx->proc ? ctx->proc->pgid : 1;
    struct yos_proc *p = yos_proc_find(ctx->rt, pid);
    return p ? p->pgid : -ESRCH;
}

int32_t yos_getpgrp(struct yos_exec_ctx *ctx)
{
    return ctx->proc ? ctx->proc->pgid : 1;
}

int32_t yos_gettid(struct yos_exec_ctx *ctx)
{
    /* One thread per guest proc — gettid() == getpid(). */
    return ctx->proc ? ctx->proc->pid : 1;
}

/* setpgid(pid=0, pgid=0): caller becomes pgrp leader of its own pgrp.
 * setpgid(pid=0, pgid=N): move caller to pgrp N (must already exist
 * in the same session, or N==caller's own pid).
 * setpgid(pid=P, pgid=N): same but for proc P (must be caller or child;
 * we don't enforce that here — userspace rarely abuses this).
 *
 * The reference is POSIX setpgid(2). Restrictions we *don't* enforce:
 *   - "session leader can't change pgid" (sid==pid for leaders)
 *   - "target must not have execve()'d since fork"
 * These are not load-bearing for the shells we run; if they become so
 * we'll add them. */
int32_t yos_setpgid(struct yos_exec_ctx *ctx, int32_t pid, int32_t pgid)
{
    ydebug("setpgid(%d, %d)\n", pid, pgid);
    if (!ctx->proc) return -ESRCH;
    struct yos_proc *target = (pid == 0)
        ? ctx->proc
        : yos_proc_find(ctx->rt, pid);
    if (!target) return -ESRCH;
    int32_t new_pgid = (pgid == 0) ? target->pid : pgid;
    if (new_pgid <= 0) return -EINVAL;
    target->pgid = new_pgid;
    return 0;
}

/* setsid: caller becomes leader of a new session and pgrp.
 * Fails if caller is already a pgrp leader (sid==pid AND someone else
 * shares the pgrp). Simpler check: forbid if pgid==pid (we're already
 * leading some pgrp). */
int32_t yos_setsid(struct yos_exec_ctx *ctx)
{
    ydebug("setsid()\n");
    if (!ctx->proc) return -ESRCH;
    /* Conservative: deny if already a pgrp leader (POSIX EPERM). */
    if (ctx->proc->pgid == ctx->proc->pid &&
        ctx->proc->sid == ctx->proc->pid) {
        /* Already a session leader: the call is idempotent in spirit;
         * Linux returns EPERM, but for simple init/shell flows we can
         * just succeed-and-return-current-sid. */
        return ctx->proc->sid;
    }
    ctx->proc->sid  = ctx->proc->pid;
    ctx->proc->pgid = ctx->proc->pid;
    /* New session has no controlling tty; kernel would also clear that.
     * We leave rt->fg_pgid alone — anything that opens /dev/tty later
     * will be the new owner if it does TIOCSCTTY. */
    return ctx->proc->sid;
}

int32_t yos_proc_arch_prctl(struct yos_exec_ctx *ctx)
{
    (void)ctx;
    ydebug("arch_prctl() = 0 (stub, not relevant for wasm32)\n");
    return 0; /* arch_prctl is x86-64 specific, not relevant for wasm32 */
}

/* ============================================================================
 * Time syscalls with 32-bit struct conversion
 * ============================================================================ */

/* 32-bit timespec (i386 uses 32-bit time_t) */
struct old_timespec32 {
    int32_t tv_sec;
    int32_t tv_nsec;
};

int32_t yos_proc_clock_gettime(struct yos_exec_ctx *ctx, int32_t clockid, uint32_t tp_addr)
{
    ydebug("clock_gettime(%d, 0x%x)\n", clockid, tp_addr);

    if (tp_addr == 0 || tp_addr >= ctx->memory_size - sizeof(struct old_timespec32)) {
        return -EFAULT;
    }

    struct timespec ts;
    int ret = clock_gettime(clockid, &ts);
    if (ret < 0) {
        ydebug("clock_gettime = %d\n", -errno);
        return -errno;
    }

    /* Convert host64 timespec to wasm32 old_timespec32 */
    struct old_timespec32 *tp = (struct old_timespec32 *)(ctx->memory + tp_addr);
    tp->tv_sec = (int32_t)ts.tv_sec;
    tp->tv_nsec = (int32_t)ts.tv_nsec;

    ydebug("clock_gettime = 0 (sec=%d, nsec=%d)\n", tp->tv_sec, tp->tv_nsec);
    return 0;
}

int32_t yos_proc_clock_getres(struct yos_exec_ctx *ctx, int32_t clockid, uint32_t res_addr)
{
    ydebug("clock_getres(%d, 0x%x)\n", clockid, res_addr);

    struct timespec ts;
    int ret = clock_getres(clockid, res_addr ? &ts : NULL);
    if (ret < 0) {
        return -errno;
    }

    if (res_addr && res_addr < ctx->memory_size - sizeof(struct old_timespec32)) {
        struct old_timespec32 *res = (struct old_timespec32 *)(ctx->memory + res_addr);
        res->tv_sec = (int32_t)ts.tv_sec;
        res->tv_nsec = (int32_t)ts.tv_nsec;
    }

    return 0;
}

int32_t yos_proc_nanosleep(struct yos_exec_ctx *ctx, uint32_t rqtp_addr, uint32_t rmtp_addr)
{
    ydebug("nanosleep(0x%x, 0x%x)\n", rqtp_addr, rmtp_addr);

    if (rqtp_addr == 0 || rqtp_addr >= ctx->memory_size - sizeof(struct old_timespec32)) {
        return -EFAULT;
    }

    /* Convert wasm32 old_timespec32 to host64 timespec */
    struct old_timespec32 *rqtp = (struct old_timespec32 *)(ctx->memory + rqtp_addr);
    struct timespec req = {
        .tv_sec = rqtp->tv_sec,
        .tv_nsec = rqtp->tv_nsec
    };
    struct timespec rem;

    int ret = nanosleep(&req, &rem);
    if (ret < 0) {
        /* On EINTR, copy remaining time */
        if (errno == EINTR && rmtp_addr && rmtp_addr < ctx->memory_size - sizeof(struct old_timespec32)) {
            struct old_timespec32 *rmtp = (struct old_timespec32 *)(ctx->memory + rmtp_addr);
            rmtp->tv_sec = (int32_t)rem.tv_sec;
            rmtp->tv_nsec = (int32_t)rem.tv_nsec;
        }
        return -errno;
    }

    return 0;
}
