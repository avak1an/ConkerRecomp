/* Minimal host compatibility for the upstream DSP interpreter. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <inttypes.h>
#define g_new0(type,n) ((type *)calloc((n), sizeof(type)))
#define g_free free
#define g_new(type,n) ((type *)malloc((n) * sizeof(type)))
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#endif
#ifndef MIN
#define MIN(a,b) ((a)<(b)?(a):(b))
#endif
#ifndef MAX
#define MAX(a,b) ((a)>(b)?(a):(b))
#endif
static inline uint32_t ldl_le_p(const void *p) { const uint8_t *b=p; return b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<24); }
static inline void stl_le_p(void *p,uint32_t v) { uint8_t *b=p; for(unsigned i=0;i<4;i++)b[i]=(uint8_t)(v>>(i*8)); }
