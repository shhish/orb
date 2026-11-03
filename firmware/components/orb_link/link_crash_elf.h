#pragma once

#include <stddef.h>

#include "orb_link_crash.h"

typedef esp_err_t (*link_crash_read_fn)(void *ctx, uint32_t offset, void *out, size_t size);
typedef bool (*link_crash_pc_fn)(uint32_t pc);

/* Offsets come from this build's FreeRTOS TCB and Xtensa exception frame. */
typedef struct {
    size_t task_name;
    size_t pc;
    size_t a0;
    size_t a1;
} link_crash_layout_t;

esp_err_t link_crash_elf_read(link_crash_read_fn read, void *ctx, uint32_t size,
                             const link_crash_layout_t *layout, link_crash_pc_fn pc_valid,
                             orb_link_crash_t *out);
