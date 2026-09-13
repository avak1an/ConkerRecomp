#ifndef CONKER_INPUT_QUEUE_H
#define CONKER_INPUT_QUEUE_H
#include "xinput_xbox.h"

/* Retain button transitions between guest polls, which can be far slower than
 * the host sampler. Axis-only changes replace the newest sample. */
typedef struct ConkerInputQueue {
    XBOX_INPUT_STATE latest, pending[8];
    unsigned head, count;
} ConkerInputQueue;
void conker_input_queue_sample(ConkerInputQueue *q, const XBOX_INPUT_STATE *s);
void conker_input_queue_read(ConkerInputQueue *q, XBOX_INPUT_STATE *s);
#endif
