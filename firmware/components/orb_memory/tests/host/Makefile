CC ?= clang
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined
BUILD ?= /tmp/orb-memory-host
COMPONENT := ../..

.PHONY: test
test:
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -pthread -I$(COMPONENT)/include test_memory_budget.c $(COMPONENT)/orb_memory.c -o $(BUILD)/test_memory_budget
	$(BUILD)/test_memory_budget
	$(CC) $(CFLAGS) -DESP_PLATFORM -Istubs -I$(COMPONENT)/include test_memory_esp.c $(COMPONENT)/orb_memory.c -o $(BUILD)/test_memory_esp
	$(BUILD)/test_memory_esp
