#pragma once

#include "voice_core.h"

/* Zero means an event-only wait. Work and BOOT dwell keep their original
 * cadence; a parked worker is woken by model, link, speech and physical input. */
static inline uint32_t voice_wait_ms(const voice_core_t *core, bool speech, bool eligible)
{
    if (core->view.phase == ORB_VOICE_RECORDING) return 1;
    bool inactive = core->view.phase == ORB_VOICE_IDLE || core->view.phase == ORB_VOICE_CONFIRMING ||
        core->view.phase == ORB_VOICE_ERROR;
    return eligible && inactive && !speech && !core->boot_held && !core->cancel_stream &&
        !core->redo_pending && !core->command_redo_pending ? 0 : 20;
}
