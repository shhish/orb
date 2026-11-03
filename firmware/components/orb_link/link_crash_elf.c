/* ELF/core-dump layout and Xtensa window unwinding adapted from Espressif
 * ESP-IDF espcoredump/core_dump_elf.c and esp_system/debug_helpers.c.
 * SPDX-FileCopyrightText: 2015-2025 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "link_crash_elf.h"

#include <string.h>

#define DUMP_HEADER 12u
#define ELF_HEADER 52u
#define PROGRAM_HEADER 32u
#define NOTE_HEADER 12u
#define EXTRA_INFO 677u

typedef struct {
    link_crash_read_fn read;
    void *ctx;
    uint32_t size;
    uint32_t phoff;
    uint16_t phnum;
} image_t;

typedef struct {
    uint32_t type;
    uint32_t offset;
    uint32_t address;
    uint32_t size;
} segment_t;

static uint32_t word(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t half(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static esp_err_t read_at(const image_t *image, uint32_t offset, void *out, size_t size)
{
    if (offset > image->size || size > image->size - offset) return ESP_ERR_INVALID_SIZE;
    return image->read(image->ctx, offset, out, size);
}

static esp_err_t segment(const image_t *image, unsigned index, segment_t *out)
{
    uint8_t ph[PROGRAM_HEADER];
    esp_err_t err = read_at(image, image->phoff + index * PROGRAM_HEADER, ph, sizeof(ph));
    if (err != ESP_OK) return err;
    uint32_t offset = word(ph + 4), size = word(ph + 16);
    if (offset > image->size - DUMP_HEADER || size > image->size - DUMP_HEADER - offset)
        return ESP_ERR_INVALID_SIZE;
    *out = (segment_t){word(ph), DUMP_HEADER + offset, word(ph + 8), size};
    return ESP_OK;
}

static esp_err_t crashed_tcb(const image_t *image, uint32_t *tcb, orb_link_crash_t *out)
{
    for (unsigned i = 0; i < image->phnum; i++) {
        segment_t ph;
        esp_err_t err = segment(image, i, &ph);
        if (err != ESP_OK) return err;
        if (ph.type != 4) continue; /* PT_NOTE */
        uint32_t used = 0;
        while (used < ph.size) {
            uint8_t note[NOTE_HEADER];
            if (ph.size - used < sizeof(note)) return ESP_ERR_INVALID_SIZE;
            err = read_at(image, ph.offset + used, note, sizeof(note));
            if (err != ESP_OK) return err;
            uint32_t name = word(note), desc = word(note + 4);
            if (name > UINT32_MAX - 3 || desc > UINT32_MAX - 3) return ESP_ERR_INVALID_SIZE;
            name = (name + 3) & ~3u;
            uint32_t padded = (desc + 3) & ~3u;
            uint32_t remain = ph.size - used - NOTE_HEADER;
            if (name > remain || padded > remain - name) return ESP_ERR_INVALID_SIZE;
            if (word(note + 8) == EXTRA_INFO) {
                if (desc < 20) return ESP_ERR_INVALID_SIZE;
                uint8_t data[20];
                err = read_at(image, ph.offset + used + NOTE_HEADER + name, data, sizeof(data));
                if (err == ESP_OK) {
                    *tcb = word(data);
                    out->cause = word(data + 8);
                    out->vaddr = word(data + 16);
                }
                return err;
            }
            used += NOTE_HEADER + name + padded;
        }
    }
    return ESP_ERR_INVALID_RESPONSE;
}

static uint32_t instruction(uint32_t pc)
{
    if (pc & 0x80000000u) pc = (pc & 0x3fffffffu) | 0x40000000u;
    return pc - 3; /* Xtensa return address points after the call instruction. */
}

static esp_err_t stack_word(const image_t *image, const segment_t *stack, size_t offset, uint32_t *out)
{
    if (offset > stack->size || 4 > stack->size - offset) return ESP_ERR_INVALID_SIZE;
    uint8_t bytes[4];
    esp_err_t err = read_at(image, stack->offset + offset, bytes, sizeof(bytes));
    if (err == ESP_OK) *out = word(bytes);
    return err;
}

static esp_err_t unwind(const image_t *image, const segment_t *stack, const link_crash_layout_t *layout,
                        link_crash_pc_fn pc_valid, orb_link_crash_t *out)
{
    uint32_t pc, sp, next;
    esp_err_t err = stack_word(image, stack, layout->pc, &pc);
    if (err == ESP_OK) err = stack_word(image, stack, layout->a0, &next);
    if (err == ESP_OK) err = stack_word(image, stack, layout->a1, &sp);
    if (err != ESP_OK) return err;
    out->pc = instruction(pc);
    out->backtrace[out->depth++] = out->pc;
    /* InstrFetchProhibited can have an invalid PC but a useful caller stack,
     * just as IDF's panic unwinder permits (Xtensa EXCCAUSE 20). */
    out->corrupted = !pc_valid(out->pc) && out->cause != 20;
    while (next && !out->corrupted && out->depth < 16) {
        /* Only follow recorded bytes, never live pointers from the saved stack.
         * This also handles crashed PSRAM stacks without an MMU mapping. */
        if ((sp & 15) || sp < stack->address || sp - stack->address < 16 ||
            sp - stack->address > stack->size) {
            out->corrupted = true;
            break;
        }
        uint32_t previous_sp = sp;
        size_t offset = sp - stack->address - 16;
        pc = instruction(next);
        err = stack_word(image, stack, offset, &next);
        if (err == ESP_OK) err = stack_word(image, stack, offset + 4, &sp);
        if (err != ESP_OK) return err;
        if (!pc_valid(pc) || (next && sp <= previous_sp)) out->corrupted = true;
        else out->backtrace[out->depth++] = pc;
    }
    return ESP_OK;
}

esp_err_t link_crash_elf_read(link_crash_read_fn read, void *ctx, uint32_t size,
                             const link_crash_layout_t *layout, link_crash_pc_fn pc_valid,
                             orb_link_crash_t *out)
{
    if (!read || !layout || !pc_valid || !out) return ESP_ERR_INVALID_ARG;
    *out = (orb_link_crash_t){0};
    image_t image = {.read = read, .ctx = ctx, .size = size};
    uint8_t header[DUMP_HEADER + ELF_HEADER];
    esp_err_t err = read_at(&image, 0, header, sizeof(header));
    if (err != ESP_OK) return err;
    const uint8_t *elf = header + DUMP_HEADER;
    if (word(header) != size || memcmp(elf, "\177ELF\1\1\1", 7) ||
        half(elf + 16) != 4 || half(elf + 18) != 94 || half(elf + 42) != PROGRAM_HEADER)
        return ESP_ERR_INVALID_RESPONSE;
    uint32_t phoff = word(elf + 28);
    image.phnum = half(elf + 44);
    if (phoff < ELF_HEADER || phoff > size - DUMP_HEADER || !image.phnum ||
        image.phnum > (size - DUMP_HEADER - phoff) / PROGRAM_HEADER) return ESP_ERR_INVALID_SIZE;
    image.phoff = DUMP_HEADER + phoff;
    uint32_t tcb = 0;
    err = crashed_tcb(&image, &tcb, out);
    if (err != ESP_OK) return err;
    bool found = false;
    for (unsigned i = 0; i < image.phnum; i++) {
        segment_t ph;
        err = segment(&image, i, &ph);
        if (err != ESP_OK) return err;
        if (ph.type != 1) continue; /* PT_LOAD: IDF stores each stack after its TCB. */
        if (found) return unwind(&image, &ph, layout, pc_valid, out);
        if (ph.address != tcb) continue;
        if (layout->task_name > ph.size || 16 > ph.size - layout->task_name) return ESP_ERR_INVALID_SIZE;
        err = read_at(&image, ph.offset + layout->task_name, out->task, 16);
        if (err != ESP_OK) return err;
        /* A damaged TCB must not turn this panic receipt into invalid UTF-8
         * or inject control characters into the retained hub log. */
        for (unsigned j = 0; j < 16 && out->task[j]; j++) {
            unsigned char ch = out->task[j];
            if (ch < 32 || ch > 126) out->task[j] = '?';
        }
        if (!out->task[0]) memcpy(out->task, "unknown", sizeof("unknown"));
        found = true;
    }
    return ESP_ERR_INVALID_RESPONSE;
}
