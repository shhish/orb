# Host tests for orb_link's ESP-free parts (envelopes and bodies, pairing code,
# backoff), built with the macOS clang: `make` builds and runs them.
#
# cJSON comes from the managed component the test_app downloads: run
# `idf.py -C ../../test_app reconfigure` once (scripts/idf.sh build does the
# same for firmware/), or set CJSON_DIR to any cJSON 1.7 source directory.

COMPONENT := ../..
CJSON_DIR ?= $(firstword $(wildcard \
	$(COMPONENT)/test_app/managed_components/espressif__cjson/cJSON \
	$(COMPONENT)/../../managed_components/espressif__cjson/cJSON))

CC := clang
SAN := -fsanitize=address,undefined -fno-sanitize-recover=all
CFLAGS := -std=c11 -O1 -g $(SAN) -I$(COMPONENT) -I$(CJSON_DIR)
WARN := -Wall -Wextra -Werror

BUILD := build
TESTS := test_main.c test_sas.c test_proto.c test_backoff.c test_control.c
UNITS := $(COMPONENT)/link_sas.c $(COMPONENT)/link_proto.c $(COMPONENT)/link_backoff.c
HEADERS := test.h $(wildcard $(COMPONENT)/link_*.h)

.PHONY: test clean

test: $(BUILD)/orb_link_tests $(BUILD)/orb_link_v11_tests $(BUILD)/orb_link_memory_tests
	$(BUILD)/orb_link_tests
	$(BUILD)/orb_link_v11_tests
	$(BUILD)/orb_link_memory_tests
	python3 test_callbacks.py "$(BUILD)"
	CJSON_DIR="$(abspath $(CJSON_DIR))" python3 test_radio.py "$(BUILD)"
	CJSON_DIR="$(abspath $(CJSON_DIR))" python3 test_delivery.py "$(BUILD)"
	CJSON_DIR="$(abspath $(CJSON_DIR))" python3 test_outgoing.py "$(BUILD)"
	CJSON_DIR="$(abspath $(CJSON_DIR))" python3 test_home_dispatch.py "$(BUILD)"
	CJSON_DIR="$(abspath $(CJSON_DIR))" python3 test_devtools.py "$(BUILD)"
	CJSON_DIR="$(abspath $(CJSON_DIR))" python3 test_hello.py "$(BUILD)"
	CJSON_DIR="$(abspath $(CJSON_DIR))" python3 test_crash.py "$(BUILD)"

$(BUILD)/orb_link_v11_tests: test_v11.c $(COMPONENT)/link_v11.c $(COMPONENT)/link_v11.h $(COMPONENT)/../orb_model/orb_model.c $(COMPONENT)/../orb_model/orb_model_voice_notify.c $(BUILD)/cJSON.o
	$(CC) $(CFLAGS) $(WARN) -pthread -I$(COMPONENT)/../orb_model/include -o $@ test_v11.c $(COMPONENT)/link_v11.c $(COMPONENT)/link_proto.c $(COMPONENT)/../orb_model/orb_model.c $(COMPONENT)/../orb_model/orb_model_voice_notify.c $(BUILD)/cJSON.o

$(BUILD)/orb_link_memory_tests: test_memory.c $(COMPONENT)/link_memory.c $(COMPONENT)/link_v11.c $(COMPONENT)/link_proto.c $(COMPONENT)/../orb_model/orb_model.c $(COMPONENT)/../orb_model/orb_model_voice_notify.c $(COMPONENT)/include/orb_link_memory.h stubs/esp_heap_caps.h $(BUILD)/cJSON.o
	$(CC) $(CFLAGS) $(WARN) -pthread -Istubs -I$(COMPONENT)/include -I$(COMPONENT)/../orb_model/include -o $@ test_memory.c $(COMPONENT)/link_memory.c $(COMPONENT)/link_v11.c $(COMPONENT)/link_proto.c $(COMPONENT)/../orb_model/orb_model.c $(COMPONENT)/../orb_model/orb_model_voice_notify.c $(BUILD)/cJSON.o

$(BUILD)/orb_link_tests: $(TESTS) $(UNITS) $(HEADERS) $(BUILD)/cJSON.o
	$(CC) $(CFLAGS) $(WARN) -o $@ $(TESTS) $(UNITS) $(BUILD)/cJSON.o

# Third-party code: no -Werror, and quiet about its sprintf (macOS deprecates it).
$(BUILD)/cJSON.o: $(CJSON_DIR)/cJSON.c
	@test -n "$(CJSON_DIR)" || { echo "cJSON not found: see the top of this Makefile"; exit 1; }
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Wno-deprecated-declarations -c -o $@ $<

clean:
	rm -rf $(BUILD)
