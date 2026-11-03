/* Validate static chrome after the actual simulator executable scrolls for 8 seconds. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#include "src/libs/lodepng/lodepng.h"

static unsigned load_png(const char *path, unsigned char **pixels, unsigned *w, unsigned *h)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 1;
    if (fseek(f, 0, SEEK_END)) { fclose(f); return 1; }
    long length = ftell(f);
    if (length <= 0 || fseek(f, 0, SEEK_SET)) { fclose(f); return 1; }
    unsigned char *data = malloc((size_t)length);
    if (!data) { fclose(f); return 1; }
    bool read_ok = fread(data, 1, (size_t)length, f) == (size_t)length;
    fclose(f);
    /* Export format is part of screenshot review: indexed PNG previews can
     * hide unchanged regions despite correct decoded panel pixels. */
    read_ok &= length >= 26 && data[24] == 8 && data[25] == 2;
    unsigned err = read_ok ? lodepng_decode32(pixels, w, h, data, (size_t)length) : 1;
    free(data);
    return err;
}

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    lv_init();
    unsigned char *a = NULL, *b = NULL;
    unsigned aw, ah, bw, bh;
    unsigned err = load_png(argv[1], &a, &aw, &ah);
    if (!err) err = load_png(argv[2], &b, &bw, &bh);
    bool ok = !err && aw == 466 && ah == 466 && bw == aw && bh == ah;
    /* This LVGL fork returns a draw-buffer object from lodepng_decode32,
     * unlike upstream LodePNG's raw byte pointer. */
    lv_draw_buf_t *abuf = (lv_draw_buf_t *)a, *bbuf = (lv_draw_buf_t *)b;
    if (ok) { a = abuf->data; b = bbuf->data; }
    int clock_white = 0, plus_white = 0;
    if (ok) {
        for (int y = 46; y <= 89; y++) {
            for (int x = 191; x <= 275; x++) {
                const unsigned char *p = &b[(y * 466 + x) * 4];
                clock_white += p[0] > 200 && p[1] > 200 && p[2] > 200;
                ok &= !memcmp(&a[(y * 466 + x) * 4], p, 3);
            }
        }
        for (int y = 84; y <= 118; y++) {
            for (int x = 348; x <= 384; x++) {
                const unsigned char *p = &b[(y * 466 + x) * 4];
                plus_white += p[0] > 200 && p[1] > 200 && p[2] > 200;
            }
        }
        ok &= clock_white > 200 && plus_white > 60;
    }
    if (abuf) lv_draw_buf_destroy(abuf);
    if (bbuf) lv_draw_buf_destroy(bbuf);
    if (!ok) fprintf(stderr, "chrome pixels changed or disappeared (PNG %u, clock %d, plus %d)\n", err, clock_white, plus_white);
    return ok ? 0 : 1;
}
