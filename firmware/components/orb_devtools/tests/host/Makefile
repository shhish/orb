CC := clang
CJSON_DIR ?= ../../../../managed_components/espressif__cjson/cJSON
CFLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all
BUILD := build
.PHONY: test clean
test: $(BUILD)/test_wire $(BUILD)/test_console_reply $(BUILD)/test_button_command $(BUILD)/test_arc_command $(BUILD)/test_tilt_command
	$(BUILD)/test_wire
	$(BUILD)/test_console_reply
	$(BUILD)/test_button_command
	$(BUILD)/test_arc_command
	$(BUILD)/test_tilt_command
	python3 test_async.py "$(BUILD)"
	python3 test_wake.py
	python3 test_bench_power.py
	python3 test_diagnostics.py
	python3 test_heap.py
	python3 test_sender.py
	python3 test_output.py "$(BUILD)"
	python3 test_wifi_deadline.py "$(BUILD)"
$(BUILD)/test_wire: test_wire.c ../../orb_devtools_wire.h $(CJSON_DIR)/cJSON.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Wno-deprecated-declarations -I../.. -I$(CJSON_DIR) test_wire.c $(CJSON_DIR)/cJSON.c -o $@
$(BUILD)/test_console_reply: test_console_reply.c ../../orb_console_reply.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I../.. test_console_reply.c -o $@
$(BUILD)/test_button_command: test_button_command.c ../../orb_button_command.h ../../../orb_buttons/include/orb_buttons.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I../.. -I../../../orb_buttons/include test_button_command.c -o $@
$(BUILD)/test_arc_command: test_arc_command.c ../../orb_arc_command.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I../.. test_arc_command.c -o $@
$(BUILD)/test_tilt_command: test_tilt_command.c ../../orb_tilt_command.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I../.. test_tilt_command.c -o $@
clean:
	rm -rf $(BUILD)
