CC ?= clang
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined
BUILD ?= build
SRC := ../../orb_sound.c ../../orb_sound_preferences.c ../../orb_sound_core.c ../../orb_sound_pcm.c test_sound.c

test: $(BUILD)/test_sound
	$(BUILD)/test_sound
	CC="$(CC)" CFLAGS="$(CFLAGS)" python3 test_voice_lease.py
	CC="$(CC)" CFLAGS="$(CFLAGS)" python3 test_preferences_storage.py

$(BUILD)/test_sound: $(SRC) ../../orb_sound_core.h ../../include/orb_sound.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I../.. -I../../include $(SRC) -pthread -lm -o $@

clean:
	rm -rf $(BUILD)

.PHONY: test clean
