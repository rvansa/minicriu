#include "mc-shared.h"
#include <linux/rseq.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>

static const size_t rseq_area_offset =
#if defined(__x86_64__)
        0x9a0;
#else
#error "Unimplemented"
#endif

static const unsigned int rseq_sig =
#if defined(__x86_64__)
        0x53053053;
#else
#error "Unimplemented"
#endif
static const size_t rseq_size = 32; // sizeof(struct rseq) is defined as always 32 bytes, aligned at 32-bytes

static bool call_rseq(pthread_t thr, bool reg) {
    void *rseq_area = (char *) thr + rseq_area_offset;
    unsigned int cpu_id = *((unsigned int *) rseq_area + 1);
    if (cpu_id == RSEQ_CPU_ID_UNINITIALIZED || cpu_id == RSEQ_CPU_ID_REGISTRATION_FAILED) {
        return true;
    }
    return !syscall(SYS_rseq, rseq_area, rseq_size, reg ? 0 : RSEQ_FLAG_UNREGISTER, rseq_sig);
}

void mc_unregister_rseq(pthread_t thr) {
    if (!call_rseq(thr, false)) {
        perror("Cannot unregister GLIBC rseq");
        exit(1);
    }
}

void mc_reregister_rseq(pthread_t thr) {
	if (!call_rseq(thr, true)) {
        perror("Cannot re-register GLIBC rseq");
        exit(1);
	}
}
