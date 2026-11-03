CC := clang
SAN ?= -fsanitize=address,undefined -fno-sanitize-recover=all
CFLAGS := -std=c11 -O1 -g $(SAN) -Wall -Wextra -Werror -Istubs -I../../include
BUILD ?= build
HEADERS := ../../include/orb_mem.h $(wildcard stubs/*.h stubs/freertos/*.h)

.PHONY: test clean

test: $(BUILD)/test_mem $(BUILD)/test_disabled
	$(BUILD)/test_mem
	$(BUILD)/test_disabled

$(BUILD)/test_mem: test_mem.c ../../orb_mem.c $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -DCONFIG_ORB_DEVTOOLS=1 -o $@ test_mem.c

$(BUILD)/test_disabled: test_disabled.c ../../orb_mem.c $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -DCONFIG_ORB_DEVTOOLS=0 -o $@ test_disabled.c ../../orb_mem.c

clean:
	rm -rf $(BUILD)
