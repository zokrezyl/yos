// YOS Types - Process and Runtime Structures
//
// This file defines the runtime structures for YOS, NOT the Linux kernel ABI.
// Linux kernel structs come from include/linux/ headers.
//
#ifndef YOS_TYPES_H
#define YOS_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Constants
// ============================================================================

#define YOS_MAX_FDS      256
#define YOS_MAX_PROCS    64
#define YOS_PATH_MAX     4096

// Asyncify states (Binaryen)
#define YOS_ASYNCIFY_NORMAL    0
#define YOS_ASYNCIFY_UNWINDING 1
#define YOS_ASYNCIFY_REWINDING 2
#define YOS_ASYNCIFY_BUF_SIZE  16384

// ============================================================================
// Process State
// ============================================================================

enum yos_proc_state {
    YOS_PROC_FREE = 0,
    YOS_PROC_READY,
    YOS_PROC_RUNNING,
    YOS_PROC_WAITING,
    YOS_PROC_ZOMBIE,
};

// Process identity (slot in process table)
struct yos_proc {
    int32_t pid;
    int32_t ppid;
    int32_t pgid;
    int32_t sid;
    enum yos_proc_state state;
    int32_t exit_code;

    pthread_mutex_t lock;
    pthread_cond_t wait_cond;
    int exited;

    pthread_t thread;
};

// ============================================================================
// File Descriptor
// ============================================================================

struct yos_fd {
    int host_fd;          // -1 = unused
    uint32_t flags;
    char path[YOS_PATH_MAX];
};

// ============================================================================
// Execution Context (per-process)
//
// Each forked process gets its own yos_exec_ctx with:
//   - Own wasm3 runtime
//   - Own linear memory (copied from parent on fork)
//   - Own fd table (copied from parent on fork)
//   - Own cwd, umask, etc.
// ============================================================================

struct yos_runtime;

struct yos_exec_ctx {
    struct yos_runtime *rt;
    struct yos_proc *proc;

    // File descriptors (per-process)
    struct yos_fd fds[YOS_MAX_FDS];

    // Filesystem state (per-process)
    char cwd[YOS_PATH_MAX];
    uint32_t umask;

    // Memory management
    uint32_t brk_current;

    // WASM runtime (wasm3) - each process has its own
    void *wasm_env;         // IM3Environment
    void *wasm_runtime;     // IM3Runtime
    void *wasm_module;      // IM3Module
    uint8_t *wasm_memory;   // Linear memory base
    uint32_t wasm_mem_size; // Linear memory size

    // Fork/asyncify state
    int is_child;
    uint32_t asyncify_ptr;
    int fork_pending;
    int32_t fork_return;
    int64_t *saved_globals;
    uint32_t saved_globals_count;
};

// ============================================================================
// Global Runtime
// ============================================================================

struct yos_runtime {
    struct yos_proc procs[YOS_MAX_PROCS];
    pthread_mutex_t proc_lock;
    int32_t next_pid;

    uint8_t *wasm_bytes;
    size_t wasm_size;

    int argc;
    char **argv;
    char **envp;
};

// ============================================================================
// Helper Macros
// ============================================================================

// Convert wasm offset to host pointer
#define YOS_PTR(ctx, offset) ((void*)((ctx)->wasm_memory + (uint32_t)(offset)))

// Bounds check wasm pointer
#define YOS_PTR_VALID(ctx, offset, len) \
    (((uint32_t)(offset) + (len)) <= (ctx)->wasm_mem_size)

#ifdef __cplusplus
}
#endif

#endif // YOS_TYPES_H
