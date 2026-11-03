#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    char task[17];
    uint32_t pc;
    uint32_t cause;
    uint32_t vaddr;
    uint32_t backtrace[16];
    uint32_t depth;
    bool corrupted;
} orb_link_crash_t;

/* Read only: never erases the saved panic. Call from an internal-stack service. */
esp_err_t orb_link_crash_read(orb_link_crash_t *out);
