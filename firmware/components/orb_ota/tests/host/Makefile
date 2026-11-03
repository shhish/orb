# Host tests for orb_ota's pure logic, with the macOS clang:  make
#
# cJSON is the copy the ESP-IDF component manager fetched: build test_app once
# (it downloads espressif/cjson into test_app/managed_components), or pass
# CJSON_DIR=<dir with cJSON.c>.

COMPONENT := ../..
CJSON_DIR ?= $(firstword $(wildcard $(COMPONENT)/test_app/managed_components/espressif__cjson/cJSON) \
                         $(wildcard $(HOME)/Library/Caches/Espressif/ComponentManager/*/espressif__cjson_*/cJSON))
CC := clang
CFLAGS := -std=c11 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer
WARN := -Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion
BUILD := build

.PHONY: test clean check-cjson
test: $(BUILD)/test_orb_ota_logic $(BUILD)/test_orb_ota_assets
	./$(BUILD)/test_orb_ota_logic
	./$(BUILD)/test_orb_ota_assets

check-cjson:
	@test -f "$(CJSON_DIR)/cJSON.c" || { echo "cJSON not found: build test_app once, or set CJSON_DIR"; exit 1; }

# cJSON is third-party: built quietly, without our warning flags.
$(BUILD)/cJSON.o: check-cjson | $(BUILD)
	$(CC) $(CFLAGS) -w -c "$(CJSON_DIR)/cJSON.c" -o $@

$(BUILD)/test_orb_ota_logic: test_orb_ota_logic.c $(COMPONENT)/orb_ota_logic.c $(COMPONENT)/orb_ota_logic.h \
                             $(BUILD)/cJSON.o | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) -I$(COMPONENT) -I"$(CJSON_DIR)" test_orb_ota_logic.c $(COMPONENT)/orb_ota_logic.c \
		$(BUILD)/cJSON.o -o $@

$(BUILD)/test_orb_ota_assets: test_orb_ota_assets.c $(COMPONENT)/orb_ota_assets.c $(COMPONENT)/orb_ota_assets.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) -I$(COMPONENT) test_orb_ota_assets.c $(COMPONENT)/orb_ota_assets.c -o $@

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)
