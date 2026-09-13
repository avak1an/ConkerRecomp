#ifndef CONKER_KERNEL_GUEST_TIMER_H
#define CONKER_KERNEL_GUEST_TIMER_H
#include <stdint.h>

void recomp_timer_init(uint32_t address, unsigned type);
int recomp_timer_set(uint32_t address, int64_t due, int32_t period_ms, uint32_t dpc);
int recomp_timer_cancel(uint32_t address);
/* Called on a guest safe point; never executes guest code on a host worker. */
void recomp_timer_poll(void);

#endif
