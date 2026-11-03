#pragma once

#include <stdbool.h>

/* Called after the queue's model lock is released; returns enqueue success. */
bool orb_model_voice_notify(bool queued);
