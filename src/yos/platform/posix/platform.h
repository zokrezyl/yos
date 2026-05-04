/* yos host platform abstraction (POSIX side).
 *
 * Cross-cutting host primitives that diverge between Linux and darwin
 * (macOS / iOS / tvOS). Concrete impls live next to this header in
 * platform-linux.c and platform-darwin.c; meson picks the right one
 * for host_machine.system() at configure time. The Windows port will
 * grow a sibling platform/windows/platform.h with the same surface.
 *
 * Add a function here when you find a Linux-only call that has a
 * darwin equivalent (or vice-versa); leave fully-portable POSIX calls
 * (open, close, pthread_create, kqueue, …) at their natural site.
 */
#ifndef YOS_PLATFORM_POSIX_PLATFORM_H
#define YOS_PLATFORM_POSIX_PLATFORM_H

#include <sys/types.h>

/* Kernel-visible thread identifier for the calling thread.
 *
 * Linux:  gettid(2)               — pid_t fits the kernel TID exactly.
 * Darwin: pthread_threadid_np(3)  — returns uint64; we truncate to
 *         pid_t for the call sites that compare/log it. The darwin
 *         id is a 64-bit monotonic counter; truncation collisions
 *         are theoretical for our use (logging, ring-buffer keys). */
pid_t yos_plat_gettid(void);

#endif /* YOS_PLATFORM_POSIX_PLATFORM_H */
