# The actual pack loader under allocation caps and injected failures.
CC ?= clang
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined
BUILD_DIR ?= /tmp/orb-fonts-host
FONTS = ../..
LVGL = ../../../../third_party/lvgl__lvgl

.PHONY: test
test:
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -DLV_CONF_INCLUDE_SIMPLE -DLV_KCONFIG_IGNORE \
	    -I. -I$(LVGL) -I$(FONTS)/include -I$(FONTS)/src \
	    test_pack_memory.c $(FONTS)/src/orb_fonts_pack.c -o $(BUILD_DIR)/test_pack_memory
	$(BUILD_DIR)/test_pack_memory
	$(CC) $(CFLAGS) -DLV_CONF_INCLUDE_SIMPLE -DLV_KCONFIG_IGNORE -DORB_FONTS_TEST_MAPPING \
	    -I. -I$(LVGL) -I$(FONTS)/include -I$(FONTS)/src \
	    test_pack_mapped.c $(FONTS)/src/orb_fonts_pack.c -o $(BUILD_DIR)/test_pack_mapped
	$(BUILD_DIR)/test_pack_mapped
	$(CC) $(CFLAGS) -I$(FONTS)/include test_assets_format.c -o $(BUILD_DIR)/test_assets_format
	$(BUILD_DIR)/test_assets_format
	$(CC) $(CFLAGS) -DLV_CONF_INCLUDE_SIMPLE -DLV_KCONFIG_IGNORE \
	    -I. -Istubs -I$(LVGL) -I$(FONTS)/include -I$(FONTS)/src \
	    test_assets_port.c -o $(BUILD_DIR)/test_assets_port
	$(BUILD_DIR)/test_assets_port
	$(CC) $(CFLAGS) -DLV_CONF_INCLUDE_SIMPLE -DLV_KCONFIG_IGNORE \
	    -I. -Istubs -I$(LVGL) -I$(FONTS)/include -I$(FONTS)/src -I$(FONTS)/../orb_fs/include \
	    test_fonts_port.c $(FONTS)/port/orb_fonts_port_esp.c -lz -o $(BUILD_DIR)/test_fonts_port
	$(BUILD_DIR)/test_fonts_port
