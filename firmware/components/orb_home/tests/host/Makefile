COMPONENT := ../..
DATA := $(COMPONENT)/../orb_home_data
MEMORY := $(COMPONENT)/../orb_memory
LINK := $(COMPONENT)/../orb_link
MODEL := $(COMPONENT)/../orb_model
CJSON_DIR ?= $(COMPONENT)/../../managed_components/espressif__cjson/cJSON
CC := clang
SAN ?= -fsanitize=address,undefined -fno-sanitize-recover=all
CFLAGS := -std=c11 -O1 -g $(SAN) -Wall -Wextra -Werror -pthread -I$(DATA)/include -I$(MEMORY)/include -I$(LINK) -I$(MODEL)/include -I$(CJSON_DIR)
BUILD ?= build
.PHONY: test clean
test: $(BUILD)/home_tests $(BUILD)/home_model_tests $(BUILD)/home_memory_tests $(BUILD)/home_wire_tests $(BUILD)/home_payload_tests $(BUILD)/home_payload_esp_tests
	$(BUILD)/home_tests
	$(BUILD)/home_model_tests
	$(BUILD)/home_memory_tests
	$(BUILD)/home_wire_tests
	$(BUILD)/home_payload_tests
	$(BUILD)/home_payload_esp_tests
$(BUILD)/memory.o: $(MEMORY)/orb_memory.c $(MEMORY)/include/orb_memory.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $(MEMORY)/orb_memory.c -o $@
$(BUILD)/home_wire_tests: $(BUILD)/memory.o test_wire.c $(wildcard ../fixtures/*.jsonl) $(DATA)/orb_home.c $(LINK)/link_home.c $(LINK)/link_proto.c $(LINK)/link_v11.c $(MODEL)/orb_model.c $(MODEL)/orb_model_voice_notify.c $(DATA)/include/orb_home.h $(LINK)/link_home.h $(LINK)/link_proto.h $(LINK)/link_v11.h $(MODEL)/include/orb_model.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -DFIXTURE_DIR='"$(abspath ../fixtures)"' -Wno-deprecated-declarations -o $@ test_wire.c $(DATA)/orb_home.c $(LINK)/link_home.c $(LINK)/link_proto.c $(LINK)/link_v11.c $(MODEL)/orb_model.c $(MODEL)/orb_model_voice_notify.c $(CJSON_DIR)/cJSON.c $(BUILD)/memory.o
$(BUILD)/home_model_tests: $(BUILD)/memory.o test_model.c $(DATA)/orb_home.c $(DATA)/include/orb_home.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_model.c $(DATA)/orb_home.c $(BUILD)/memory.o
$(BUILD)/home_tests: $(BUILD)/memory.o test_home.c $(DATA)/orb_home.c $(LINK)/link_home.c $(DATA)/include/orb_home.h $(LINK)/link_home.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Wno-deprecated-declarations -o $@ test_home.c $(DATA)/orb_home.c $(LINK)/link_home.c $(CJSON_DIR)/cJSON.c $(BUILD)/memory.o
$(BUILD)/home_memory_tests: $(BUILD)/memory.o test_memory.c $(DATA)/orb_home.c $(DATA)/include/orb_home.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -DESP_PLATFORM -I$(MODEL)/tests/host/stubs -o $@ test_memory.c $(DATA)/orb_home.c $(BUILD)/memory.o
$(BUILD)/home_payload_tests: test_payload.c $(COMPONENT)/orb_home_payload.c $(COMPONENT)/orb_home_payload.h $(BUILD)/memory.o
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPONENT) -o $@ test_payload.c $(COMPONENT)/orb_home_payload.c $(BUILD)/memory.o
$(BUILD)/home_payload_esp_tests: test_payload.c $(COMPONENT)/orb_home_payload.c $(COMPONENT)/orb_home_payload.h $(BUILD)/memory.o
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -DESP_PLATFORM -I$(COMPONENT) -I$(MODEL)/tests/host/stubs -o $@ test_payload.c $(COMPONENT)/orb_home_payload.c $(BUILD)/memory.o
clean:
	rm -rf $(BUILD)
