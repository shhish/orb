# Host tests for orb_board's ESP-free logic (macOS clang): make test
CC ?= clang
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O1 -g
SRC := ../../orb_imu.c test_imu.c

test: test_imu test_senses test_battery test_audio test_panel_timing test_panel_power
	./test_imu
	./test_senses
	./test_battery
	python3 test_mic_gain.py
	python3 test_qmi_control.py
	python3 test_button_edges.py
	python3 test_sound_idle.py

test_senses: ../../orb_senses.c test_senses.c ../../include/orb_senses.h
	$(CC) $(CFLAGS) -I../../include ../../orb_senses.c test_senses.c -lm -o $@

test_battery: ../../orb_battery.c test_battery.c ../../include/orb_battery.h
	$(CC) $(CFLAGS) -I../../include ../../orb_battery.c test_battery.c -lm -o $@

test_imu: $(SRC) ../../orb_imu.h
	$(CC) $(CFLAGS) -I../.. $(SRC) -lm -o $@

test_audio: test_audio.c test_audio.py ../../board_audio.c
	CC="$(CC)" CFLAGS="$(CFLAGS)" python3 test_audio.py

test_panel_timing: test_panel_timing.py ../../board_ws175c.c
	CC="$(CC)" python3 test_panel_timing.py

test_panel_power: test_panel_power.py ../../board_panel_power.c ../../orb_panel_power.h
	CC="$(CC)" python3 test_panel_power.py

clean:
	rm -rf test_imu test_senses test_battery *.dSYM

.PHONY: test test_audio test_panel_timing test_panel_power clean
