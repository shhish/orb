/* Rotated glyph ink must remain ordered against overlapping render workers. */
#include <stdio.h>
#include <stdlib.h>

#include "lvgl.h"
#include "src/draw/lv_draw_private.h"

static unsigned s_checks;

static void check(bool condition, const char *why)
{
    s_checks++;
    if (condition) return;
    fprintf(stderr, "letter task bounds: %s\n", why);
    exit(EXIT_FAILURE);
}

static bool glyph(const lv_font_t *font, lv_font_glyph_dsc_t *dsc,
                  uint32_t letter, uint32_t next)
{
    (void)font;
    (void)next;
    if (letter != 'A') return false;
    dsc->adv_w = 12;
    dsc->box_w = 8;
    dsc->box_h = 4;
    dsc->format = LV_FONT_GLYPH_FORMAT_A8;
    return true;
}

static const lv_font_t s_font = {.get_glyph_dsc = glyph, .line_height = 60, .base_line = 10};

static void overlapping_worker(int32_t rotation, lv_point_t ink)
{
    /* Outside a display, construct the production task and ask the real
     * scheduler before either worker starts drawing. */
    lv_layer_t layer = {0};
    layer.opa = LV_OPA_COVER;
    layer.color_format = LV_COLOR_FORMAT_RGB565;
    layer.buf_area = layer._clip_area = (lv_area_t){0, 0, 199, 199};
    lv_draw_letter_dsc_t letter;
    lv_draw_letter_dsc_init(&letter);
    letter.font = &s_font;
    letter.unicode = 'A';
    letter.rotation = rotation;
    lv_draw_letter(&layer, &letter, &(lv_point_t){100, 100});
    lv_draw_task_t *task = layer.draw_task_head;
    check(task && task->type == LV_DRAW_TASK_TYPE_LETTER, "production letter task missing");
    check(task->area.x1 == 100 && task->area.y1 == 100, "renderer anchor moved");
    check(task->_real_area.x1 <= ink.x && task->_real_area.x2 >= ink.x &&
          task->_real_area.y1 <= ink.y && task->_real_area.y2 >= ink.y,
          "task dependency bounds exclude real glyph ink");
    task->state = LV_DRAW_TASK_STATE_WAITING;
    task->preferred_draw_unit_id = 1;
    lv_draw_task_t background = {
        .state = LV_DRAW_TASK_STATE_IN_PROGRESS, .preferred_draw_unit_id = 2,
        ._real_area = {ink.x, ink.y, ink.x, ink.y}, .next = task,
    };
    layer.draw_task_head = &background;
    check(lv_draw_get_next_available_task(&layer, NULL, 1) == NULL,
          "overlapping worker was allowed to race the glyph");
    background.state = LV_DRAW_TASK_STATE_FINISHED;
    check(lv_draw_get_next_available_task(&layer, NULL, 1) == task,
          "completed overlap still blocks glyph");
    background.state = LV_DRAW_TASK_STATE_IN_PROGRESS;
    background._real_area = (lv_area_t){0, 0, 4, 4};
    check(lv_draw_get_next_available_task(&layer, NULL, 1) == task,
          "unrelated region lost parallel rendering");
    layer.draw_task_head = task;
    lv_draw_layer_deinit(&layer);
}

int main(void)
{
    lv_init();
    overlapping_worker(0, (lv_point_t){98, 98});
    overlapping_worker(900, (lv_point_t){102, 98});
    overlapping_worker(1800, (lv_point_t){102, 102});
    overlapping_worker(2700, (lv_point_t){98, 102});
    lv_deinit();
    printf("letter task bounds: %u checks passed\n", s_checks);
    return EXIT_SUCCESS;
}
