CC ?= clang
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined
MODEL := ../../../orb_model/include
SRC := ../../voice_core.c test_voice.c
BUILD ?= build

test: $(BUILD)/test_voice $(BUILD)/test_speech
	$(BUILD)/test_voice
	$(BUILD)/test_speech
	python3 test_requests.py
	python3 test_events.py

$(BUILD)/test_voice: $(SRC) ../../voice_core.h $(MODEL)/orb_model.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I../.. -I$(MODEL) $(SRC) -o $@

clean:
	rm -rf $(BUILD)

.PHONY: test clean

$(BUILD)/test_speech: ../../speech_core.c ../../mic_dsp.c test_speech.c ../../speech_core.h ../../mic_dsp.h ../../../orb_board/include/orb_mic_levels.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I../.. -I../../../orb_board/include ../../speech_core.c ../../mic_dsp.c test_speech.c -o $@ -lm
