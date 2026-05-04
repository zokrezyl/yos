/* Darwin host impls of the POSIX platform abstraction. */
#include "platform.h"

#include <pthread.h>
#include <stdint.h>

pid_t yos_plat_gettid(void) {
    uint64_t tid64 = 0;
    pthread_threadid_np(NULL, &tid64);
    return (pid_t)tid64;
}
