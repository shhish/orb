# LVGL/ESP-free bounded-store tests. `make` builds and runs under ASan/UBSan.
# Optional: make BUILD=build-tsan SAN=-fsanitize=thread
COMPONENT := ../..
CC := clang
SAN ?= -fsanitize=address,undefined -fno-sanitize-recover=all
CFLAGS := -std=c99 -O1 -g $(SAN) -Wall -Wextra -Werror -pthread -I$(COMPONENT)/include -I$(COMPONENT)/../orb_fonts/include
BUILD ?= build

.PHONY: test clean

test: $(BUILD)/orb_model_tests $(BUILD)/orb_model_memory_tests
	$(BUILD)/orb_model_tests
	$(BUILD)/orb_model_memory_tests

$(BUILD)/orb_model_tests: test_model.c $(COMPONENT)/orb_model.c $(COMPONENT)/orb_model_voice_notify.c $(COMPONENT)/include/orb_model.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_model.c $(COMPONENT)/orb_model.c $(COMPONENT)/orb_model_voice_notify.c

$(BUILD)/orb_model_memory_tests: test_memory.c $(COMPONENT)/orb_model.c $(COMPONENT)/include/orb_model.h $(wildcard stubs/*.h stubs/freertos/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -DESP_PLATFORM -Istubs -o $@ test_memory.c $(COMPONENT)/orb_model.c

clean:
	rm -rf $(BUILD)
