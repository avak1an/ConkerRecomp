#include "input_queue.h"
#include <string.h>

static unsigned actions(const XBOX_INPUT_STATE *s)
{
    unsigned bits = s->Gamepad.wButtons;
    for (unsigned i = 0; i < 8; ++i)
        if (s->Gamepad.bAnalogButtons[i] > XBOX_ANALOG_BUTTON_THRESHOLD)
            bits |= 1u << (8 + i);
    if (s->Gamepad.sThumbLX < -16000) bits |= 1u << 16;
    if (s->Gamepad.sThumbLX >  16000) bits |= 1u << 17;
    if (s->Gamepad.sThumbLY < -16000) bits |= 1u << 18;
    if (s->Gamepad.sThumbLY >  16000) bits |= 1u << 19;
    return bits;
}

void conker_input_queue_sample(ConkerInputQueue *q, const XBOX_INPUT_STATE *s)
{
    if (actions(s) != actions(&q->latest)) {
        /* A long burst must not leave seconds of queued movement or a stuck
         * held button. On overflow discard the backlog and keep current input. */
        if (q->count == 8) q->head = q->count = 0;
        q->pending[(q->head + q->count++) % 8] = *s;
    } else if (q->count) {
        q->pending[(q->head + q->count - 1) % 8] = *s;
    }
    q->latest = *s;
}

void conker_input_queue_read(ConkerInputQueue *q, XBOX_INPUT_STATE *s)
{
    if (q->count) {
        *s = q->pending[q->head];
        q->head = (q->head + 1) % 8;
        --q->count;
    } else {
        *s = q->latest;
    }
}
