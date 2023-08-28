#ifndef MC_SHARED_H
#define MC_SHARED_H

#include <pthread.h>

void mc_unregister_rseq(pthread_t thr);
void mc_reregister_rseq(pthread_t thr);

#endif // MC_SHARED_H
