/* Linux host impls of the POSIX platform abstraction. */
#include "platform.h"

#include <sys/syscall.h>
#include <unistd.h>

pid_t yos_plat_gettid(void) {
    return (pid_t)syscall(SYS_gettid);
}
