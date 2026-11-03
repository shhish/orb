/* Exercise the real SIMD dispatch wrapper with a strict host kernel. The
 * assembly kernel's scalar unaligned fragments must never be selected. */
#include "orb_simd_copy.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint32_t opacity;
    void *destination;
    uint32_t width, height, destination_stride;
    const void *source;
    uint32_t source_stride;
    const void *mask;
    uint32_t mask_stride;
} kernel_descriptor_t;

static unsigned s_calls;

int lv_rgb565_blend_normal_to_rgb565_esp(kernel_descriptor_t *descriptor)
{
    assert(descriptor->width && !(descriptor->width & 7));
    assert(!((uintptr_t)descriptor->destination & 15));
    assert(!((uintptr_t)descriptor->source & 1));
    assert(descriptor->height == 1);
    assert(descriptor->destination_stride == descriptor->width * 2);
    assert(descriptor->source_stride == descriptor->width * 2);
    memcpy(descriptor->destination, descriptor->source, descriptor->width * 2);
    s_calls++;
    return 1;
}

int lv_color_blend_to_rgb565_esp(kernel_descriptor_t *descriptor)
{
    (void)descriptor;
    assert(!"Fill is outside this copy test");
    return 0;
}

int main(void)
{
    uint8_t source[1088] __attribute__((aligned(16)));
    uint8_t destination[1088] __attribute__((aligned(16)));
    for (unsigned i = 0; i < sizeof(source); i++) source[i] = (uint8_t)(i * 71 + 113);
    unsigned cases = 0;
    for (unsigned from = 0; from < 32; from++) {
        for (unsigned to = 0; to < 32; to++) {
            for (unsigned count = 0; count <= 513; count++) {
                memset(destination, 0xA5, sizeof(destination));
                orb_simd_copy_rgb565(destination + to, source + from, count);
                assert(!memcmp(destination + to, source + from, count * 2));
                for (unsigned i = 0; i < to; i++) assert(destination[i] == 0xA5);
                for (unsigned i = to + count * 2; i < sizeof(destination); i++)
                    assert(destination[i] == 0xA5);
                cases++;
            }
        }
    }
    assert(s_calls);
    printf("SIMD copy: %u alignment/length cases; %u aligned vector dispatches\n", cases, s_calls);
    return 0;
}
