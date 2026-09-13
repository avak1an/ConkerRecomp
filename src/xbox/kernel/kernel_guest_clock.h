#ifndef KERNEL_GUEST_CLOCK_H
#define KERNEL_GUEST_CLOCK_H
#include <stdint.h>
/* The loader owns this aligned DWORD and must stop the clock before unmapping. */
int recomp_clock_start(volatile uint32_t *tick_count);
void recomp_clock_stop(void);
#endif
