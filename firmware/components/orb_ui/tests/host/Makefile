# Host tests for orb_ui's ESP-free and LVGL-free logic (macOS clang): make test
CC ?= clang
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O1 -g

test: test_sdf test_motion test_blur_reclaim
	./test_sdf
	./test_motion

test_blur_reclaim:
	python3 test_blur_reclaim.py

test_sdf: ../../orb_glass_sdf.c test_sdf.c ../../orb_glass_sdf.h
	$(CC) $(CFLAGS) -I../.. ../../orb_glass_sdf.c test_sdf.c -lm -o $@

test_motion: ../../orb_motion_math.c test_motion.c ../../orb_motion_math.h
	$(CC) $(CFLAGS) -I../.. ../../orb_motion_math.c test_motion.c -lm -o $@

clean:
	rm -rf test_sdf test_motion *.dSYM

.PHONY: test clean test_blur_reclaim
