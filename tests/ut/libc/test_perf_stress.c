/*
 * test_perf_stress.c — combined performance + stress test.
 *
 * WHAT this verifies AND measures:
 *   1. fork() + waitpid() — process spawn round-trip latency.
 *   2. fork() + execve(argv[0]) + waitpid() — full image-replacement
 *      round-trip (re-execs the SAME wasm with argv[1]="child", so the
 *      re-loaded copy detects itself and exits immediately).
 *   3. pthread_create() + pthread_join() — host-pthread round-trip,
 *      with per-thread file I/O.
 *   4. open/write/close/unlink on the host fs — sustained single-
 *      threaded I/O throughput through yos's vfs bridges.
 *
 * Each phase records wall-clock via clock_gettime(CLOCK_MONOTONIC) and
 * prints us-total + us-per-op to stdout. Numbers are observational —
 * the test only asserts that every operation succeeded (every fork
 * returned, every join returned, every execve produced exit 0, every
 * write returned the requested byte count). A failure in any of those
 * is a stress-test fail; the timings are informational.
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
 *   This is the only test that combines fork + pthread + execve + I/O
 *   in one process. It surfaces interactions between the asyncify fork
 *   dance, the host-pthread implementation, the fd-map fork-dup, and
 *   the linear-memory reset on execve — exactly the surface that
 *   regressed every time we touched the proc table or the allocator.
 *
 * Expected: exit 0, stdout contains "perf-stress ok".
 */

#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>
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

    /* ---- 2. fork + execve(argv[0]) + exit ---------------------- */
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

    /* ---- 4. file I/O throughput -------------------------------- */
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
