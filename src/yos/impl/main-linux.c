/* impl/main-linux.c — Linux-only host signal infrastructure.
 *
 * Linux delivers synchronous CPU faults straight to the BSD signal
 * handler — no Mach exception port indirection. So `yos_mach_install
 * _exc_handler` is a no-op here, and `yos_main_install_altstack` is
 * just sigaltstack(2).
 *
 * NO #ifdef inside this file — meson selects it only on linux hosts.
 */

#include <signal.h>
#include <stddef.h>
#include <stdint.h>

void yos_main_install_altstack(void *sp, size_t sz)
{
    stack_t ss = {0};
    ss.ss_sp = sp;
    ss.ss_size = sz;
    ss.ss_flags = 0;
    (void)sigaltstack(&ss, NULL);
}

void yos_mach_install_exc_handler(void)
{
    /* No Mach on Linux. */
}
