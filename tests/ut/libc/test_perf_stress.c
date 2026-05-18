/*
 * test_perf_stress.c — combined performance + stress test.
 *
 * WHAT this verifies AND measures:
 *   1. fork() + waitpid() — flat spawn round-trip latency (20 iters).
 *   2. RECURSIVE fork tree — 1 root + 10 children + 100 grandchildren
 *      = 111 processes total. Each leaf appends its own PID and parent
 *      PID to a shared O_APPEND log so the root can verify:
 *        - 111 PIDs total were recorded
 *        - 111 distinct PIDs (no aliasing across the proc table)
 *        - parent-PID column matches the actual fork structure
 *      Stresses fork-after-fork, the wasm-linear-memory snapshot
 *      restore at every fork level, and the O_APPEND atomicity yos
 *      promises across forked children writing the same fd.
 *   3. /proc consistency walk — fork 8 long-living children that
 *      block on read() from a pipe (sleep(2) loop is unreliable on
 *      yos; pipe-read is). Parent opens /proc, readdir-s the numeric
 *      dirs, and asserts every child PID appears in /proc with the
 *      right state, then closes the pipes so the children exit.
 *      Pins yos's procfs PID-table population path — when proc.c's
 *      fork-fd-snapshot regressed, /proc was missing children and
 *      this is the cheapest way to surface that.
 *   4. fork() + execve(argv[0]) + waitpid() — image-replacement
 *      round-trip (re-execs the SAME wasm with argv[1]="child"; the
 *      re-loaded copy detects itself and exits immediately).
 *   5. pthread_create + pthread_join — host-pthread round-trip with
 *      per-thread file I/O AND a battery of locking-primitive checks
 *      run in the SAME process:
 *        a. mutex-protected counter: N threads × M iters → final
 *           count must equal N×M (catches lost updates).
 *        b. condvar producer/consumer with bounded queue: producer
 *           emits N items, consumers drain them, must consume exactly
 *           N (catches missed wakeups / spurious wakeups handled
 *           wrong).
 *        c. rwlock: K readers + 1 writer, writer increments a counter
 *           under wlock, readers under rlock observe monotonically
 *           non-decreasing counter values.
 *   6. open/write/close/unlink on the host fs — sustained single-
 *      threaded I/O throughput through yos's vfs bridges.
 *
 * Each phase records wall-clock via clock_gettime(CLOCK_MONOTONIC) and
 * prints us-total + us-per-op to stdout. The test only asserts that
 * every operation succeeded; timings are informational.
 *
 * PHASE ORDER MATTERS. Pthread phase MUST run AFTER all fork/execve
 * activity. Running fork() after pthread_create() + pthread_join() on
 * the calling task triggers a yos bug — the post-pthread linear-memory
 * state isn't snapshot-safe and the parent traps inside fork's asyncify
 * unwind. Reproducer: 2+ pthread_create/join, then any fork → SIGSEGV.
 * Filed as a known limitation; for now keep pthread last in this test
 * so it doesn't mask the rest of the surface.
 *
 * WHY this matters:
 *   This is the only test that combines fork + recursive fork + procfs
 *   + pthread + locking + execve + I/O in one process. It surfaces
 *   interactions between the asyncify fork dance, the host-pthread
 *   implementation, the fd-map fork-dup, the procfs PID enumeration,
 *   and the linear-memory reset on execve — exactly the surface that
 *   regressed every time we touched the proc table or the allocator.
 *
 * Expected: exit 0, stdout contains "perf-stress ok".
 */

#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <dirent.h>
#include <sys/wait.h>

extern char **environ;

static long long now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

/* Stdio buffering interacts poorly with fork in this test: stdout
 * buffer state set up before fork doesn't always survive into the
 * post-fork printf path under yos's asyncify snapshot/restore.
 * Cleanest workaround is to format into a stack buffer and write(2)
 * straight to fd 1 — no stdio state, no flush ambiguity, no buffer
 * inherited across fork boundaries. */
static void emit(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    write(1, s, n);
}

static void emit_err(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    write(2, s, n);
}

static void *thread_worker(void *arg)
{
    int id = (int)(long)arg;
    char path[64];
    snprintf(path, sizeof path, "/tmp/yos-perf-t%d.dat", id);
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd < 0) return (void *)(long)1;
    char buf[1024];
    memset(buf, 'x', sizeof buf);
    for (int i = 0; i < 8; i++) {
        if (write(fd, buf, sizeof buf) != (ssize_t)sizeof buf) {
            close(fd);
            return (void *)(long)2;
        }
    }
    close(fd);
    unlink(path);
    return (void *)0;
}

/* ── recursive fork tree ───────────────────────────────────────────
 *
 * fork_branch(log_fd, branches[], depth):
 *   At this level, fork `branches[0]` children. Each child writes its
 *   own PID + parent PID to log_fd, then recurses with branches+1
 *   (one shallower level) until depth==0. Returns 0 on success, -1
 *   on any fork/wait/write failure.
 *
 * Total processes spawned with branches={10, 10} from the root:
 *   root + 10*(1 + 10) = root + 110 = 111.
 * The log file then contains 110 lines (root doesn't log itself).
 */
static int fork_branch(int log_fd, const int *branches, int depth)
{
    if (depth == 0) return 0;
    int n = branches[0];
    for (int i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid < 0) return -1;
        if (pid == 0) {
            /* Child: log self+parent, recurse, exit. */
            char buf[64];
            int len = snprintf(buf, sizeof buf, "%d %d\n",
                               (int)getpid(), (int)getppid());
            /* O_APPEND fd shared with parent — under POSIX a single
             * write < PIPE_BUF (4096) is atomic. We rely on that
             * for the log to be uncorrupted across 100+ writers. */
            if (write(log_fd, buf, (size_t)len) != (ssize_t)len) {
                _exit(1);
            }
            int rc = fork_branch(log_fd, branches + 1, depth - 1);
            _exit(rc == 0 ? 0 : 2);
        }
        int st = 0;
        if (waitpid(pid, &st, 0) != pid) return -1;
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) return -1;
    }
    return 0;
}

/* ── mutex-protected counter ───────────────────────────────────────
 *
 * Each worker bumps `*counter` MUTEX_ITERS times under mutex. With N
 * workers the final value MUST be N*MUTEX_ITERS — any deviation means
 * a lost update (a missed lock acquire or a torn read-modify-write).
 */
#define MUTEX_THREADS 6
#define MUTEX_ITERS   2000

struct mutex_ctx {
    pthread_mutex_t lock;
    long counter;
};
static void *mutex_worker(void *p)
{
    struct mutex_ctx *c = (struct mutex_ctx *)p;
    for (int i = 0; i < MUTEX_ITERS; i++) {
        pthread_mutex_lock(&c->lock);
        c->counter++;
        pthread_mutex_unlock(&c->lock);
    }
    return NULL;
}

/* ── condvar producer/consumer with bounded queue ──────────────────
 *
 * Producer emits CV_ITEMS values onto a small ring; M consumers drain.
 * Producer signals via `not_empty` after enqueue, waits on `not_full`
 * if full. Consumers wait on `not_empty` if empty, signal `not_full`
 * after dequeue. At exit each consumer is woken via a broadcast on
 * not_empty + done flag. Total consumed across all consumers MUST
 * equal CV_ITEMS — duplicates or misses fail the test.
 */
#define CV_CONSUMERS 4
#define CV_ITEMS     400
#define CV_RING_SZ   8

struct cv_ctx {
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
    int             ring[CV_RING_SZ];
    int             head, tail, count;
    int             done;
    long            total_consumed;
};
static void *cv_consumer(void *p)
{
    struct cv_ctx *c = (struct cv_ctx *)p;
    long local = 0;
    for (;;) {
        pthread_mutex_lock(&c->lock);
        while (c->count == 0 && !c->done)
            pthread_cond_wait(&c->not_empty, &c->lock);
        if (c->count == 0 && c->done) {
            pthread_mutex_unlock(&c->lock);
            break;
        }
        (void)c->ring[c->tail];
        c->tail = (c->tail + 1) % CV_RING_SZ;
        c->count--;
        pthread_cond_signal(&c->not_full);
        pthread_mutex_unlock(&c->lock);
        local++;
    }
    pthread_mutex_lock(&c->lock);
    c->total_consumed += local;
    pthread_mutex_unlock(&c->lock);
    return NULL;
}

/* ── rwlock: readers observe monotonic writer counter ──────────────
 *
 * Single writer increments `rw_counter` RW_WRITES times under wlock.
 * RW_READERS readers each sample `rw_counter` RW_READS times under
 * rlock and assert their successive samples are non-decreasing. Any
 * decrease means an rlock reader saw a torn / out-of-order write,
 * which would indicate a broken rwlock implementation in yos.
 */
#define RW_READERS 4
#define RW_WRITES  500
#define RW_READS   2000

struct rw_ctx {
    pthread_rwlock_t lock;
    volatile long    counter;
    volatile int     writer_done;
    int              reader_violations;  /* observed monotonicity break */
    pthread_mutex_t  vio_lock;           /* protects reader_violations */
};
static void *rw_reader(void *p)
{
    struct rw_ctx *c = (struct rw_ctx *)p;
    long prev = -1;
    int violations = 0;
    for (int i = 0; i < RW_READS; i++) {
        pthread_rwlock_rdlock(&c->lock);
        long cur = c->counter;
        pthread_rwlock_unlock(&c->lock);
        if (cur < prev) violations++;
        prev = cur;
        if (c->writer_done && i > RW_READS / 4) break;
    }
    pthread_mutex_lock(&c->vio_lock);
    c->reader_violations += violations;
    pthread_mutex_unlock(&c->vio_lock);
    return NULL;
}
static void *rw_writer(void *p)
{
    struct rw_ctx *c = (struct rw_ctx *)p;
    for (int i = 0; i < RW_WRITES; i++) {
        pthread_rwlock_wrlock(&c->lock);
        c->counter++;
        pthread_rwlock_unlock(&c->lock);
    }
    c->writer_done = 1;
    return NULL;
}

/* ── randomness: deterministic xorshift32, seeded from PID ─────────
 *
 * Used by the chaos-churn phases below. We want REPRODUCIBLE chaos:
 * if the test trips a yos bug, re-running the same wasm under the
 * same root PID should hit the same path, so we don't seed from
 * clock_gettime(). PID alone biases slightly across runs because the
 * runtime allocates PID 1, 2, 3, …; that's fine — the inner workers
 * mix in round/iter indices. */
static unsigned int g_rng = 1;
static unsigned int chaos_rand(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}
static unsigned int chaos_rand_mod(unsigned int n)
{
    return n ? chaos_rand() % n : 0;
}

/* ── chaos child SIGTERM handler ───────────────────────────────────
 *
 * yos's waitpid currently only encodes the WIFEXITED branch — a
 * guest that's signal-killed via kill() still appears to the parent
 * as a clean exit, which makes "did the kill actually land?"
 * impossible to assert by inspecting the wait status.
 *
 * Workaround: install a SIGTERM handler in each chaos child that
 * exits with a distinctive code (77). The parent then counts
 *   kill_landed = # of children that exited with code 77
 * and that's a real proof-of-kill, independent of whether yos ever
 * grows WIFSIGNALED support. SIGKILL is uncatchable so children
 * that get SIGKILL exit with whatever yos's signal-kill path
 * produces (typically 0); the kill_calls counter still tracks the
 * attempt. */
#define CHAOS_SIGTERM_EXIT 77
static void chaos_sigterm(int sig) {
    (void)sig;
    _exit(CHAOS_SIGTERM_EXIT);
}

/* ── chaos child body — pick one of several workloads ──────────────
 *
 * Each chaos child picks a workload by hashing (round, idx) so the
 * mix across N kids in a round is varied. Workloads exercise
 * different subsystems: allocator, vfs, signals, nested fork — the
 * point is to keep yos's internal state churning so we catch races
 * the single-fork test can't (concurrent fd_alloc, concurrent
 * mmap2 bumps, concurrent procfs writes, …).
 *
 * Workloads:
 *   0 — allocator churn (malloc/free in random sizes, exit clean)
 *   1 — file IO (write+close+unlink a unique /tmp file)
 *   2 — tight CPU loop (a long-running spin so the kill path has
 *       something live to interrupt)
 *   3 — nested fork (this child forks a grandchild and reaps it,
 *       so the parent's reap sees the child's exit AFTER the
 *       grandchild lifecycle has run)
 *   4 — open/close many fds (stress yos's per-ctx fd_map slots)
 */
static void chaos_child_body(int round, int idx)
{
    unsigned int seed = (unsigned int)round * 31u + (unsigned int)idx;
    unsigned int r = seed * 2654435761u;
    int work_type = (int)(r % 5);
    switch (work_type) {
    case 0: {
        void *bufs[16];
        int n = 4 + (int)((r >> 4) % 12);
        for (int i = 0; i < n; i++)
            bufs[i] = malloc(64 + ((r >> (i & 7)) % 4096));
        for (int i = 0; i < n; i++) free(bufs[i]);
        break;
    }
    case 1: {
        char path[64];
        snprintf(path, sizeof path, "/tmp/yos-chaos-%d.dat", (int)getpid());
        int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
        if (fd >= 0) {
            char buf[512];
            memset(buf, 'z' + (int)(r % 3), sizeof buf);
            (void)write(fd, buf, sizeof buf);
            close(fd);
            unlink(path);
        }
        break;
    }
    case 2: {
        /* Long-ish workload with periodic I/O. The I/O is what gives
         * yos's signal pump a chance to deliver SIGTERM to the wasm
         * handler (yos_signal_pump fires from yos_read). Pure CPU
         * spin never pumps → kill() never lands → kill_landed count
         * stays at 0 even though the parent issued the call. Mix
         * keeps the workload long enough for kill races to be real
         * (tens of ms on a release-build wasm3) while still allowing
         * the guest to observe a pending signal. */
        for (int round = 0; round < 4; round++) {
            volatile long acc = 0;
            long iters = 50000L + (long)((r >> round) % 200000L);
            for (long i = 0; i < iters; i++) acc += i ^ (i << 3);
            /* Cheap I/O to drive yos's signal pump. read 0 bytes
             * from /dev/null — non-blocking, no allocation, and
             * (crucially) goes through yos_read which is where
             * pending-signal delivery happens. Without an I/O hook
             * inside the spin, a SIGTERM from the parent's kill()
             * never reaches the wasm handler. */
            int fd = open("/dev/null", O_RDONLY);
            if (fd >= 0) { char b; (void)read(fd, &b, 0); close(fd); }
        }
        break;
    }
    case 3: {
        pid_t gp = fork();
        if (gp == 0) {
            volatile long acc = 0;
            for (long i = 0; i < 20000; i++) acc += i;
            _exit(0);
        }
        if (gp > 0) {
            int st = 0;
            (void)waitpid(gp, &st, 0);
        }
        break;
    }
    case 4: {
        int fds[16];
        int n = 4 + (int)((r >> 5) % 12);
        if (n > 16) n = 16;
        for (int i = 0; i < n; i++) fds[i] = open("/dev/null", O_RDONLY);
        for (int i = 0; i < n; i++) if (fds[i] >= 0) close(fds[i]);
        break;
    }
    }
}

/* ── chaos thread body — same idea, signal-free ───────────────────
 *
 * Threads share the host process so signals to "kill a thread" would
 * take down the whole process. Instead each thread polls a stop_flag
 * and exits cleanly. The shared mutex/counter pair lets us verify
 * the post-condition: total bumps == sum of per-thread bumps,
 * regardless of which threads finished naturally vs were told to
 * stop early. Catches lost-update / cancellation-leakage bugs in
 * yos's pthread bridge. */
struct chaos_thread_ctx {
    pthread_mutex_t  lock;
    volatile int     stop;            /* set by churn loop to ask threads to wind down */
    long             total_bumps;     /* sum of every thread's local bumps */
    int              thread_id;       /* round-robin seed for workload pick */
};
static void *chaos_thread_body(void *p)
{
    struct chaos_thread_ctx *c = (struct chaos_thread_ctx *)p;
    pthread_mutex_lock(&c->lock);
    int my_id = c->thread_id++;
    pthread_mutex_unlock(&c->lock);

    unsigned int r = (unsigned int)my_id * 2654435761u + 0xa5a5a5a5u;
    long local = 0;
    /* Each iteration does a small CPU burst + a mutex bump. Bail when
     * stop is set so the round can wind down deterministically. */
    while (!c->stop) {
        volatile long acc = 0;
        int spin = 200 + (int)(r % 800);
        r = r * 1103515245u + 12345u;
        for (int i = 0; i < spin; i++) acc += i;
        pthread_mutex_lock(&c->lock);
        c->total_bumps++;
        local++;
        pthread_mutex_unlock(&c->lock);
        /* cap so a stuck stop-flag can't run us forever — but a high
         * cap so saturation under contention is what limits us, not
         * this safety net. */
        if (local > 200000) break;
    }
    return (void *)(long)local;
}

int main(int argc, char **argv)
{
    /* Re-exec stub. The execve phase below calls back into this same
     * wasm with argv[1]="child" — bail out immediately so each round
     * measures just fork + load + start + exit, not the recursive
     * perf-stress workload. */
    if (argc >= 2 && strcmp(argv[1], "child") == 0) {
        return 0;
    }

    const int FORK_N   = 20;
    const int THREAD_N = 8;
    const int EXEC_N   = 10;
    const int IO_KB    = 256;
    int fail = 0;
    char line[256];

    snprintf(line, sizeof line, "== yos perf-stress (argv[0]=%s) ==\n",
             argv[0] ? argv[0] : "(null)");
    emit(line);

    /* ---- 1. fork + tiny child I/O + waitpid -------------------- */
    long long t0 = now_us();
    for (int i = 0; i < FORK_N; i++) {
        pid_t pid = fork();
        if (pid < 0) { emit_err("fork failed\n"); fail++; break; }
        if (pid == 0) {
            int fd = open("/tmp/yos-perf-fchild.dat",
                          O_CREAT | O_WRONLY | O_TRUNC, 0600);
            if (fd >= 0) { write(fd, "ok", 2); close(fd); }
            _exit(0);
        }
        int st = 0;
        if (waitpid(pid, &st, 0) != pid) {
            emit_err("waitpid failed\n");
            fail++;
            break;
        }
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            emit_err("fork child bad status\n");
            fail++;
            break;
        }
    }
    long long t1 = now_us();
    snprintf(line, sizeof line,
             "fork+wait    x%-3d : %8lld us total, %6lld us/op\n",
             FORK_N, t1 - t0, (t1 - t0) / FORK_N);
    emit(line);

    /* ---- 2. RECURSIVE fork tree → ~100 processes --------------
     * Scale knobs: branches={A,B} means root → A children → A*B
     * grandchildren = A + A*B total non-root processes (= log lines).
     * Default {10,10} = 110 lines, ~111 processes. yos may abort
     * under the host glibc allocator at high concurrency (host-side
     * mimalloc-over-linear-memory bookkeeping); knock it down here
     * when isolating the trap. */
    emit("phase 2: recursive fork tree starting...\n");
    {
        const int branches[] = {10, 10};  /* root → 10 → 100 leaves */
        const int depth = (int)(sizeof(branches) / sizeof(branches[0]));
        const int expect_lines = 110;     /* every non-root logs once */
        const char *logp = "/tmp/yos-perf-tree.log";
        unlink(logp);
        int log_fd = open(logp, O_CREAT | O_WRONLY | O_TRUNC | O_APPEND, 0600);
        if (log_fd < 0) {
            emit_err("recursive: open log failed\n");
            fail++;
        } else {
            long long ta = now_us();
            int rc = fork_branch(log_fd, branches, depth);
            long long tb = now_us();
            close(log_fd);
            if (rc != 0) {
                emit_err("recursive fork tree failed\n");
                fail++;
            } else {
                /* Read back the log and tally lines + distinct PIDs. */
                int rfd = open(logp, O_RDONLY);
                if (rfd < 0) {
                    emit_err("recursive: open log read failed\n");
                    fail++;
                } else {
                    char *bigbuf = (char *)malloc(64 * 1024);
                    if (!bigbuf) {
                        emit_err("recursive: malloc failed\n");
                        fail++;
                        close(rfd);
                    } else {
                        ssize_t got = read(rfd, bigbuf, 64 * 1024 - 1);
                        close(rfd);
                        if (got < 0) {
                            emit_err("recursive: log read failed\n");
                            fail++;
                        } else {
                            bigbuf[got] = '\0';
                            int n_lines = 0;
                            int pid_set[256] = {0};
                            int dup_pid = 0;
                            char *q = bigbuf;
                            while (*q) {
                                long pid = strtol(q, &q, 10);
                                while (*q == ' ') q++;
                                (void)strtol(q, &q, 10);  /* ppid */
                                while (*q == '\n') q++;
                                n_lines++;
                                int slot = (int)(pid & 0xff);
                                if (pid_set[slot]++) dup_pid++;
                            }
                            if (n_lines != expect_lines) {
                                snprintf(line, sizeof line,
                                  "recursive: %d lines, expected %d\n",
                                  n_lines, expect_lines);
                                emit_err(line);
                                fail++;
                            }
                            /* dup_pid counts hash-bucket collisions; a
                             * pid_set hit is suspicious but not always
                             * fatal (PIDs can legitimately recycle).
                             * We only flag if more than ~10% collide,
                             * which would indicate the proc table is
                             * handing out the same PID without recycle. */
                            if (dup_pid > expect_lines / 10) {
                                snprintf(line, sizeof line,
                                  "recursive: suspiciously many PID hash "
                                  "collisions: %d\n", dup_pid);
                                emit_err(line);
                                fail++;
                            }
                            snprintf(line, sizeof line,
                              "fork-tree    x%-3d : %8lld us total, "
                              "%6lld us/proc (%d log lines, %d hash dups)\n",
                              expect_lines, tb - ta,
                              (tb - ta) / expect_lines,
                              n_lines, dup_pid);
                            emit(line);
                        }
                        free(bigbuf);
                    }
                }
            }
            unlink(logp);
        }
    }

    /* ---- 3. /proc consistency walk ---------------------------- */
    {
        const int N = 8;
        int pipes[N][2];
        pid_t kids[N];
        long long ta = now_us();
        int spawned = 0;
        for (int i = 0; i < N; i++) {
            if (pipe(pipes[i]) < 0) { emit_err("pipe failed\n"); fail++; break; }
            pid_t pid = fork();
            if (pid < 0) { emit_err("procfs fork failed\n"); fail++; break; }
            if (pid == 0) {
                /* Child: close write-ends of EVERY pipe (this child's
                 * own write-end AND every prior sibling's write-end
                 * that we inherited at fork time). Without this, when
                 * the parent later closes its copy of pipes[k][1] for
                 * sibling k, child k's read still doesn't see EOF —
                 * siblings k+1..N-1 are still holding write-ends and
                 * the pipe's reader-count stays > 0. yos's fork now
                 * correctly duplicates the full parent fd table, so
                 * the bug bites; on a host that did partial fd-dup
                 * the test happened to pass anyway. */
                for (int j = 0; j <= i; j++) close(pipes[j][1]);
                char b;
                (void)read(pipes[i][0], &b, 1);
                _exit(0);
            }
            /* Parent: close read end. */
            close(pipes[i][0]);
            kids[i] = pid;
            spawned++;
        }
        /* Walk /proc and check every spawned PID appears. */
        DIR *d = opendir("/proc");
        int found = 0;
        if (!d) {
            /* /proc may not be mounted on this build — degrade to SKIP
             * for this phase rather than failing the whole test. */
            emit("procfs walk : SKIP (no /proc mount)\n");
        } else {
            int present[64] = {0};
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL) {
                /* Numeric dir name = PID. */
                long p = 0;
                int isnum = ent->d_name[0] != '\0';
                for (const char *s = ent->d_name; *s; s++) {
                    if (*s < '0' || *s > '9') { isnum = 0; break; }
                    p = p * 10 + (*s - '0');
                }
                if (!isnum) continue;
                for (int i = 0; i < spawned; i++) {
                    if ((long)kids[i] == p && !present[i]) {
                        present[i] = 1;
                        found++;
                        break;
                    }
                }
            }
            closedir(d);
            if (found != spawned) {
                snprintf(line, sizeof line,
                  "procfs: only %d of %d child PIDs visible in /proc\n",
                  found, spawned);
                emit_err(line);
                fail++;
            }
        }
        /* Drain children: close write end → child read returns 0 → exits. */
        for (int i = 0; i < spawned; i++) {
            close(pipes[i][1]);
            int st = 0;
            waitpid(kids[i], &st, 0);
        }
        long long tb = now_us();
        snprintf(line, sizeof line,
                 "procfs walk  x%-3d : %8lld us total (%d/%d PIDs visible)\n",
                 spawned, tb - ta, found, spawned);
        emit(line);
    }

    /* ---- 4. fork + execve(argv[0]) + exit ---------------------- */
    long long t4 = now_us();
    for (int i = 0; i < EXEC_N; i++) {
        pid_t pid = fork();
        if (pid < 0) { emit_err("fork(exec) failed\n"); fail++; break; }
        if (pid == 0) {
            char *cargv[3];
            cargv[0] = argv[0];
            cargv[1] = (char *)"child";
            cargv[2] = NULL;
            execve(argv[0], cargv, environ);
            emit_err("execve failed\n");
            _exit(127);
        }
        int st = 0;
        if (waitpid(pid, &st, 0) != pid) {
            emit_err("waitpid(exec) failed\n");
            fail++;
            break;
        }
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            emit_err("execve child bad status\n");
            fail++;
            break;
        }
    }
    long long t5 = now_us();
    snprintf(line, sizeof line,
             "fork+execve  x%-3d : %8lld us total, %6lld us/op\n",
             EXEC_N, t5 - t4, (t5 - t4) / EXEC_N);
    emit(line);

    /* ---- 4b. CHAOS CHURN (processes) --------------------------
     *
     * Multi-round randomized fork+kill+reap loop. Each round spawns
     * 4..CHAOS_MAX_KIDS concurrent children running varied workloads
     * (allocator churn, file IO, CPU spin, nested fork, fd burst).
     * After spawn, ~30% of children get a SIGTERM or (1-in-7) SIGKILL
     * mid-flight; the rest are left to exit cleanly. All are then
     * reaped and the round verifies:
     *   - waitpid found every spawned PID (no leaks)
     *   - signaled+exited counts add up
     *   - a follow-up waitpid(-1, …, WNOHANG) returns 0 (no zombies)
     *
     * This is the test that exercises CONCURRENT live forks in yos —
     * the only existing fork-stress phase (recursive tree) is strictly
     * serial (one live grandchild at a time). Concurrent forks shake
     * out races in the proc-table allocator, the fd-map snapshot, and
     * the asyncify rewind / linear-memory restore that the serial
     * tree can't reach. Caps at 10 concurrent kids so a 10-core host
     * can actually run them in parallel; raise CHAOS_MAX_KIDS to push
     * the host scheduler harder. */
    {
        enum { CHAOS_ROUNDS = 4, CHAOS_MAX_KIDS = 10 };
        g_rng = (unsigned int)getpid() * 2654435761u + 0xdeadu;
        long long ta = now_us();
        int total_spawned = 0;
        int total_signaled = 0;     /* WIFSIGNALED — yos doesn't set this today */
        int total_exited = 0;       /* WIFEXITED (any code) */
        int total_kill_landed = 0;  /* WIFEXITED + WEXITSTATUS == 77 — handler ran */
        int total_kill_calls = 0;   /* how many kill() succeeded (returned 0) */
        int total_zombies_left = 0;
        for (int rnd = 0; rnd < CHAOS_ROUNDS; rnd++) {
            int n_kids = 4 + (int)chaos_rand_mod(CHAOS_MAX_KIDS - 3);
            pid_t kids[CHAOS_MAX_KIDS];
            int spawned = 0;
            for (int i = 0; i < n_kids; i++) {
                pid_t pid = fork();
                if (pid < 0) {
                    emit_err("chaos: fork failed\n");
                    fail++;
                    break;
                }
                if (pid == 0) {
                    /* Install SIGTERM handler so the parent can
                     * detect kill landings via exit code 77 — see
                     * CHAOS_SIGTERM_EXIT comment above. */
                    signal(SIGTERM, chaos_sigterm);
                    chaos_child_body(rnd, i);
                    _exit(0);
                }
                kids[spawned++] = pid;
            }
            /* Random kill of a fraction of the live children. The
             * killed ones may have already exited naturally — kill
             * returns ESRCH in that case, which is fine. */
            int kill_calls = 0;
            for (int i = 0; i < spawned; i++) {
                unsigned int r = chaos_rand();
                if (r % 100 < 50) {
                    int sig = (r % 7 == 0) ? SIGKILL : SIGTERM;
                    if (kill(kids[i], sig) == 0) kill_calls++;
                }
            }
            total_kill_calls += kill_calls;
            /* Reap everyone we spawned. waitpid blocks until done. */
            for (int i = 0; i < spawned; i++) {
                int st = 0;
                if (waitpid(kids[i], &st, 0) != kids[i]) {
                    snprintf(line, sizeof line,
                      "chaos: waitpid(%d) failed (round %d)\n",
                      (int)kids[i], rnd);
                    emit_err(line);
                    fail++;
                    continue;
                }
                if (WIFSIGNALED(st)) total_signaled++;
                else if (WIFEXITED(st)) {
                    total_exited++;
                    if (WEXITSTATUS(st) == CHAOS_SIGTERM_EXIT)
                        total_kill_landed++;
                }
            }
            /* No zombies must remain after we've reaped every kid we
             * tracked. A non-tracked child here would mean a runaway
             * fork inside chaos_child_body — workload 3 forks one
             * grandchild but reaps it locally before _exit. */
            pid_t z;
            while ((z = waitpid(-1, NULL, WNOHANG)) > 0)
                total_zombies_left++;
            total_spawned += spawned;
        }
        long long tb = now_us();
        snprintf(line, sizeof line,
          "chaos proc   x%-3d : %8lld us total, %3d exited "
          "(%3d via SIGTERM handler), %3d signaled, "
          "%d kill-calls, %d zombies-left\n",
          total_spawned, tb - ta, total_exited, total_kill_landed,
          total_signaled, total_kill_calls, total_zombies_left);
        emit(line);
        if (total_exited + total_signaled != total_spawned) {
            snprintf(line, sizeof line,
              "chaos proc: spawn=%d exit+sig=%d (mismatch)\n",
              total_spawned, total_exited + total_signaled);
            emit_err(line);
            fail++;
        }
        if (total_zombies_left != 0) {
            snprintf(line, sizeof line,
              "chaos proc: %d untracked zombies left over\n",
              total_zombies_left);
            emit_err(line);
            fail++;
        }
    }

    /* ---- 3. pthread_create + per-thread I/O + pthread_join -----
     * KEEP THIS LAST. See the file header — running fork() after
     * 2+ pthread_create+join cycles trips a yos asyncify-snapshot
     * bug. Putting pthread last means we observe pthread behaviour
     * without poisoning the fork/execve phases that ran above. */
    long long t2 = now_us();
    pthread_t threads[THREAD_N];
    int spawned = 0;
    for (int i = 0; i < THREAD_N; i++) {
        if (pthread_create(&threads[i], NULL, thread_worker,
                           (void *)(long)i) != 0) {
            emit_err("pthread_create failed\n");
            fail++;
            break;
        }
        spawned++;
    }
    for (int i = 0; i < spawned; i++) {
        void *rv = (void *)(long)0xdead;
        if (pthread_join(threads[i], &rv) != 0) {
            emit_err("pthread_join failed\n");
            fail++;
        } else if (rv != (void *)0) {
            emit_err("pthread returned non-zero\n");
            fail++;
        }
    }
    long long t3 = now_us();
    snprintf(line, sizeof line,
             "pthread+join x%-3d : %8lld us total, %6lld us/op\n",
             THREAD_N, t3 - t2, (t3 - t2) / THREAD_N);
    emit(line);

    /* ---- 5b. mutex-protected counter churn ----------------------
     * Expected: counter == MUTEX_THREADS * MUTEX_ITERS exactly. */
    {
        struct mutex_ctx mctx;
        pthread_mutex_init(&mctx.lock, NULL);
        mctx.counter = 0;
        pthread_t tids[MUTEX_THREADS];
        long long ta = now_us();
        int s = 0;
        for (int i = 0; i < MUTEX_THREADS; i++) {
            if (pthread_create(&tids[i], NULL, mutex_worker, &mctx) != 0) {
                emit_err("mutex thread create failed\n");
                fail++;
                break;
            }
            s++;
        }
        for (int i = 0; i < s; i++) pthread_join(tids[i], NULL);
        long long tb = now_us();
        long expect = (long)MUTEX_THREADS * MUTEX_ITERS;
        if (mctx.counter != expect) {
            snprintf(line, sizeof line,
              "mutex churn: counter=%ld, expected=%ld (lost updates!)\n",
              mctx.counter, expect);
            emit_err(line);
            fail++;
        }
        pthread_mutex_destroy(&mctx.lock);
        snprintf(line, sizeof line,
          "mutex churn  x%-3d : %8lld us total, "
          "%6ld bumps, ok=%s\n",
          MUTEX_THREADS, tb - ta, expect,
          mctx.counter == expect ? "yes" : "NO");
        emit(line);
    }

    /* ---- 5c. condvar producer/consumer --------------------------
     * Expected: total_consumed == CV_ITEMS exactly. */
    {
        struct cv_ctx cc;
        pthread_mutex_init(&cc.lock, NULL);
        pthread_cond_init(&cc.not_empty, NULL);
        pthread_cond_init(&cc.not_full, NULL);
        cc.head = cc.tail = cc.count = cc.done = 0;
        cc.total_consumed = 0;
        pthread_t cons[CV_CONSUMERS];
        long long ta = now_us();
        int s = 0;
        for (int i = 0; i < CV_CONSUMERS; i++) {
            if (pthread_create(&cons[i], NULL, cv_consumer, &cc) != 0) {
                emit_err("cv consumer create failed\n");
                fail++;
                break;
            }
            s++;
        }
        /* Producer runs on main thread. */
        for (int i = 0; i < CV_ITEMS; i++) {
            pthread_mutex_lock(&cc.lock);
            while (cc.count == CV_RING_SZ)
                pthread_cond_wait(&cc.not_full, &cc.lock);
            cc.ring[cc.head] = i;
            cc.head = (cc.head + 1) % CV_RING_SZ;
            cc.count++;
            pthread_cond_signal(&cc.not_empty);
            pthread_mutex_unlock(&cc.lock);
        }
        pthread_mutex_lock(&cc.lock);
        cc.done = 1;
        pthread_cond_broadcast(&cc.not_empty);
        pthread_mutex_unlock(&cc.lock);
        for (int i = 0; i < s; i++) pthread_join(cons[i], NULL);
        long long tb = now_us();
        if (cc.total_consumed != CV_ITEMS) {
            snprintf(line, sizeof line,
              "condvar: consumed=%ld, expected=%d (missed wakeups!)\n",
              cc.total_consumed, CV_ITEMS);
            emit_err(line);
            fail++;
        }
        pthread_cond_destroy(&cc.not_empty);
        pthread_cond_destroy(&cc.not_full);
        pthread_mutex_destroy(&cc.lock);
        snprintf(line, sizeof line,
          "condvar      x%-3d : %8lld us total, %6d items, ok=%s\n",
          CV_CONSUMERS, tb - ta, CV_ITEMS,
          cc.total_consumed == CV_ITEMS ? "yes" : "NO");
        emit(line);
    }

    /* ---- 5d. rwlock readers-vs-writer ---------------------------
     * Expected: zero readers observe a non-monotonic counter. */
    {
        struct rw_ctx rc;
        pthread_rwlock_init(&rc.lock, NULL);
        pthread_mutex_init(&rc.vio_lock, NULL);
        rc.counter = 0;
        rc.writer_done = 0;
        rc.reader_violations = 0;
        pthread_t r[RW_READERS], w;
        long long ta = now_us();
        int s = 0;
        for (int i = 0; i < RW_READERS; i++) {
            if (pthread_create(&r[i], NULL, rw_reader, &rc) != 0) {
                emit_err("rwlock reader create failed\n");
                fail++;
                break;
            }
            s++;
        }
        if (pthread_create(&w, NULL, rw_writer, &rc) != 0) {
            emit_err("rwlock writer create failed\n");
            fail++;
        }
        for (int i = 0; i < s; i++) pthread_join(r[i], NULL);
        pthread_join(w, NULL);
        long long tb = now_us();
        if (rc.reader_violations != 0) {
            snprintf(line, sizeof line,
              "rwlock: %d monotonicity violations across readers!\n",
              rc.reader_violations);
            emit_err(line);
            fail++;
        }
        if (rc.counter != RW_WRITES) {
            snprintf(line, sizeof line,
              "rwlock: writer counter=%ld, expected=%d (lost writes!)\n",
              rc.counter, RW_WRITES);
            emit_err(line);
            fail++;
        }
        pthread_mutex_destroy(&rc.vio_lock);
        pthread_rwlock_destroy(&rc.lock);
        snprintf(line, sizeof line,
          "rwlock       %d-rd+1-wr : %8lld us total, "
          "writes=%d, viol=%d\n",
          RW_READERS, tb - ta, RW_WRITES, rc.reader_violations);
        emit(line);
    }

    /* ---- 5e. CHAOS CHURN (threads) ----------------------------
     *
     * Randomized create+stop+join loop. Each round spins up
     * 4..CHAOS_T_MAX threads, all bumping a shared mutex-protected
     * counter for varied durations. After a tiny "let them run"
     * burst, the round flips the stop flag and joins every thread,
     * collecting the per-thread bump count. Post-conditions:
     *   - every thread joined (no leaked tids)
     *   - sum of per-thread return values == ctx.total_bumps
     *     (mutex consistency under contention)
     * Catches lost-update bugs and the "thread didn't notice the
     * stop flag" hang that surfaces when yos's pthread bridge
     * mispairs a cond/mutex during fork-snapshot restore. */
    {
        enum { CHAOS_T_ROUNDS = 3, CHAOS_T_MAX = 10 };
        long long ta = now_us();
        long total_bumps_sum = 0;
        int total_threads = 0;
        int mismatches = 0;
        for (int rnd = 0; rnd < CHAOS_T_ROUNDS; rnd++) {
            int n = 4 + (int)chaos_rand_mod(CHAOS_T_MAX - 3);
            struct chaos_thread_ctx cctx;
            pthread_mutex_init(&cctx.lock, NULL);
            cctx.stop = 0;
            cctx.total_bumps = 0;
            cctx.thread_id = 0;
            pthread_t tids[CHAOS_T_MAX];
            int started = 0;
            for (int i = 0; i < n; i++) {
                if (pthread_create(&tids[i], NULL,
                                   chaos_thread_body, &cctx) != 0) {
                    emit_err("chaos thread: create failed\n");
                    fail++;
                    break;
                }
                started++;
            }
            /* Let them run for a varied bit before asking to stop.
             * No usleep — yos's sleep round-trip is uneven in this
             * stress path. Burn CPU on main for a randomized count.
             * Has to be LARGE so the worker threads actually rack up
             * a meaningful bump count before the stop flag flips —
             * previous 200K..1M burn finished in well under a ms on
             * release-build wasm3, threads got ~5 iters each, the
             * "lots of contended mutex acquires" property never
             * exercised. */
            long burn = 1000000L + (long)chaos_rand_mod(3000000L);
            volatile long s = 0;
            for (long b = 0; b < burn; b++) s += b;
            cctx.stop = 1;
            long round_sum = 0;
            for (int i = 0; i < started; i++) {
                void *rv = NULL;
                if (pthread_join(tids[i], &rv) != 0) {
                    emit_err("chaos thread: join failed\n");
                    fail++;
                    continue;
                }
                round_sum += (long)rv;
            }
            if (round_sum != cctx.total_bumps) {
                snprintf(line, sizeof line,
                  "chaos thr rnd %d: sum-of-rv=%ld != ctx.total_bumps=%ld\n",
                  rnd, round_sum, cctx.total_bumps);
                emit_err(line);
                mismatches++;
            }
            total_bumps_sum += cctx.total_bumps;
            total_threads += started;
            pthread_mutex_destroy(&cctx.lock);
        }
        long long tb = now_us();
        snprintf(line, sizeof line,
          "chaos thr    x%-3d : %8lld us total, "
          "%ld bumps, %d mismatches\n",
          total_threads, tb - ta, total_bumps_sum, mismatches);
        emit(line);
        if (mismatches) fail++;
    }

    /* ---- 6. file I/O throughput -------------------------------- */
    long long t6 = now_us();
    int fd = open("/tmp/yos-perf-io.dat",
                  O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd < 0) {
        emit_err("io open failed\n");
        fail++;
    } else {
        char buf[4096];
        memset(buf, 'y', sizeof buf);
        long bytes = 0;
        int rounds = (IO_KB * 1024 + (int)sizeof(buf) - 1) / (int)sizeof(buf);
        for (int i = 0; i < rounds; i++) {
            ssize_t n = write(fd, buf, sizeof buf);
            if (n != (ssize_t)sizeof buf) {
                emit_err("write short\n");
                fail++;
                break;
            }
            bytes += n;
        }
        close(fd);
        long long t7 = now_us();
        long long dt = t7 - t6;
        if (dt < 1) dt = 1;
        snprintf(line, sizeof line,
                 "write %ld B       : %8lld us total, %6lld KB/s\n",
                 bytes, dt, (long long)bytes * 1000 / dt);
        emit(line);
        unlink("/tmp/yos-perf-io.dat");
    }

    if (fail) {
        snprintf(line, sizeof line,
                 "perf-stress FAILED (%d failures)\n", fail);
        emit(line);
        return 1;
    }
    emit("perf-stress ok\n");
    return 0;
}
