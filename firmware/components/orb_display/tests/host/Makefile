# Host tests for orb_display's ESP-free logic (macOS clang): make test
CC ?= clang
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O1 -g
PYTHON ?= python3
SRC := ../../orb_display_geom.c test_geom.c

test: test_geom test_pipeline test_double_buffer test_draw_scratch test_draw_metadata test_lvgl_heap test_draw_profile test_update_accounting test_display_drain test_ui_fairness test_touch_poll test_scroll_trace test_pacing_driver test_cpu test_cpu_driver test_cadence test_slow
	./test_geom
	./test_pipeline
	./test_cpu
	./test_slow

test_slow: test_slow.c ../../include/orb_display_slow.h
	$(CC) $(CFLAGS) -fsanitize=address,undefined -I../../include test_slow.c -o $@

test_cpu: test_cpu.c ../../include/orb_display_cpu.h
	$(CC) $(CFLAGS) -I../../include test_cpu.c -lm -o $@

test_cadence:
	CC="$(CC)" $(PYTHON) test_cadence.py

test_cpu_driver:
	CC="$(CC)" $(PYTHON) test_cpu_driver.py

test_geom: $(SRC) ../../orb_display_geom.h
	$(CC) $(CFLAGS) -I../.. $(SRC) -lm -o $@

test_pipeline: test_pipeline.c ../../orb_display_pipeline.h
	$(CC) $(CFLAGS) -I../.. test_pipeline.c -o $@

test_double_buffer: test_double_buffer.py test_double_buffer.c test_render_accounting.c ../../orb_display.c
	CC="$(CC)" $(PYTHON) test_double_buffer.py

test_draw_scratch:
	CC="$(CC)" $(PYTHON) test_draw_scratch.py

test_draw_metadata:
	CC="$(CC)" $(PYTHON) test_draw_metadata.py

test_lvgl_heap:
	CC="$(CC)" $(PYTHON) test_lvgl_heap.py

test_draw_profile:
	CC="$(CC)" $(PYTHON) test_draw_profile.py

test_update_accounting:
	CC="$(CC)" $(PYTHON) test_update_accounting.py

test_display_drain:
	CC="$(CC)" $(PYTHON) test_display_drain.py

test_ui_fairness:
	CC="$(CC)" $(PYTHON) test_ui_fairness.py

test_touch_poll:
	CC="$(CC)" $(PYTHON) test_touch_poll.py

test_scroll_trace:
	CC="$(CC)" $(PYTHON) test_scroll_trace.py

test_pacing_driver:
	CC="$(CC)" $(PYTHON) test_pacing_driver.py

# Optional: use the installed IDF Python (with kconfiglib), without idf.py.
test_kconfig:
	$(PYTHON) test_kconfig.py

clean:
	rm -rf test_geom test_pipeline test_cpu test_slow *.dSYM

.PHONY: test test_double_buffer test_draw_scratch test_draw_metadata test_draw_profile test_update_accounting test_display_drain test_ui_fairness test_touch_poll test_scroll_trace test_pacing_driver test_cpu_driver test_cadence test_kconfig clean
