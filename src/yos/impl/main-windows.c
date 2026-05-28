/* impl/main-windows.c — Windows-host signal infrastructure no-ops +
 * one-shot debug-CRT calming.
 *
 * No sigaltstack and no Mach exception ports on Windows. yos delivers
 * synchronous CPU faults via Windows SEH (Structured Exception
 * Handling), which is wired up elsewhere — this slice just satisfies
 * the main.c link surface declared in impl/main-internal.h.
 *
 * On startup we also disable the debug CRT's invalid-parameter
 * assertion and abort()-message box. Without this, msvcrt's _close /
 * _read / _write on a non-CRT fd (e.g. a Winsock SOCKET that wasn't
 * routed through closesocket) raises a debug dialog and exits with
 * status 3 even when callers handled the EBADF return.
 *
 * NO #ifdef in this file — meson selects it only on windows hosts.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <crtdbg.h>
#include <signal.h>

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>

static void noop_invalid_parameter(const wchar_t *expression,
                                   const wchar_t *function,
                                   const wchar_t *file,
                                   unsigned int   line,
                                   uintptr_t      pReserved)
{
    (void)expression; (void)function; (void)file;
    (void)line; (void)pReserved;
}

void yos_main_install_altstack(void *sp, size_t sz)
{
    (void)sp; (void)sz;
}

void yos_mach_install_exc_handler(void)
{
    /* Calm the debug CRT — these are process-wide one-shot calls. */
    _set_invalid_parameter_handler(noop_invalid_parameter);
    _set_thread_local_invalid_parameter_handler(noop_invalid_parameter);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR,  _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR,  _CRTDBG_FILE_STDERR);
    /* SIGABRT default exits 3; install SIG_IGN so a stray abort
     * doesn't bypass our own exit-code propagation either. */
    signal(SIGABRT, SIG_IGN);

    /* Initialise Winsock once at process startup so any wasm guest's
     * gethostname / socketpair / getaddrinfo bridge succeeds — Winsock
     * APIs return WSANOTINITIALISED before WSAStartup runs. */
    {
        WSADATA d;
        (void)WSAStartup(MAKEWORD(2, 2), &d);
    }
}
