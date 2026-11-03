/* Nearest-neighbour magnification preserves the simulator's actual pixels. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "src/libs/lodepng/lodepng.h"

typedef struct {
    const char *name;
    unsigned x, y, w, h;
} crop_t;

int main(int argc, char **argv)
{
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "usage: glass_crops INPUT.png OUTPUT_PREFIX [bezel]\n");
        return 2;
    }
    lv_init();
    unsigned char *rgb = NULL;
    unsigned w, h;
    FILE *input = fopen(argv[1], "rb");
    if (!input) return 1;
    if (fseek(input, 0, SEEK_END) != 0) return 1;
    long size = ftell(input);
    if (size <= 0 || fseek(input, 0, SEEK_SET) != 0) return 1;
    unsigned char *png = malloc((size_t)size);
    if (!png) return 1;
    bool read_ok = fread(png, 1, (size_t)size, input) == (size_t)size;
    fclose(input);
    unsigned err = read_ok ? lodepng_decode32(&rgb, &w, &h, png, (size_t)size) : 1;
    free(png);
    if (err) return 1;
    /* LVGL's LodePNG port returns a draw buffer, with raw RGBA in data. */
    lv_draw_buf_t *decoded = (lv_draw_buf_t *)rgb;
    rgb = decoded->data;
    const crop_t crops[] = {
        {"reply-edge", 130, 354, 80, 84},
        {"circle-edge", 60, 60, 84, 84},
        {"shadow", 64, 118, 90, 44},
        {"bezel", 144, 440, 180, 26},
    };
    int failed = 0;
    for (unsigned i = 0; i < sizeof(crops) / sizeof(crops[0]); i++) {
        crop_t c = crops[i];
        if (argc == 4 ? strcmp(argv[3], c.name) != 0 : strcmp(c.name, "bezel") == 0) continue;
        if (c.x + c.w > w || c.y + c.h > h) {
            failed = 1;
            break;
        }
        unsigned zw = c.w * 8, zh = c.h * 8;
        unsigned char *zoom = malloc((size_t)zw * zh * 3);
        if (!zoom) {
            failed = 1;
            break;
        }
        for (unsigned y = 0; y < zh; y++) {
            for (unsigned x = 0; x < zw; x++) {
                for (unsigned channel = 0; channel < 3; channel++) {
                    zoom[(y * zw + x) * 3 + channel] = rgb[((c.y + y / 8) * w + c.x + x / 8) * 4 + channel];
                }
            }
        }
        char path[1024];
        snprintf(path, sizeof(path), "%s-%s-8x.png", argv[2], c.name);
        unsigned char *encoded = NULL;
        size_t encoded_size = 0;
        err = lodepng_encode24(&encoded, &encoded_size, zoom, zw, zh);
        if (!err) {
            FILE *output = fopen(path, "wb");
            bool wrote = output && fwrite(encoded, 1, encoded_size, output) == encoded_size;
            if (output && fclose(output) != 0) wrote = false;
            if (!wrote) err = 1;
        }
        lv_free(encoded);
        free(zoom);
        if (err) failed = 1;
    }
    lv_draw_buf_destroy(decoded);
    return failed;
}
