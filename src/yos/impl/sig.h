#ifndef YOS_SIG_H
#define YOS_SIG_H

#include "yos/types.h"

int32_t yos_sig_rt_sigaction(struct yos_exec_ctx *ctx, int32_t signum, uint32_t act, uint32_t oldact, uint32_t sigsetsize);
int32_t yos_sig_rt_sigprocmask(struct yos_exec_ctx *ctx, int32_t how, uint32_t set, uint32_t oset, uint32_t sigsetsize);
int32_t yos_sigaction(struct yos_exec_ctx *ctx, int32_t signum, uint32_t act, uint32_t oldact);
int32_t yos_sigprocmask(struct yos_exec_ctx *ctx, int32_t how, uint32_t set, uint32_t oset);
int32_t yos_pthread_sigmask(struct yos_exec_ctx *ctx, int32_t how, uint32_t set, uint32_t oset);

#endif /* YOS_SIG_H */
