#include <stdio.h>
#include <stdlib.h>
#include <zlib.h>
#include "orb_fonts_private.h"

bool orb_fonts_port_read(uint32_t offset, void *dst, size_t size)
{
    FILE *file = fopen(ORB_FONT_PACK_PATH, "rb");
    if (!file) return false;
    bool ok = fseek(file, (long)offset, SEEK_SET) == 0 && fread(dst, 1, size, file) == size;
    fclose(file);
    return ok;
}

bool orb_fonts_port_inflate(void *dst, size_t dst_size, const void *src, size_t src_size)
{
    uLongf actual = dst_size;
    return uncompress(dst, &actual, src, src_size) == Z_OK && actual == dst_size;
}

void *orb_fonts_port_alloc(size_t size) { return malloc(size); }
void orb_fonts_port_free(void *ptr) { free(ptr); }
