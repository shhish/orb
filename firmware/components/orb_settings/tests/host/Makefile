CC := clang
CFLAGS := -std=c11 -D_POSIX_C_SOURCE=200809L -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer
WARN := -Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion
COMPONENT := ../..
FS := ../../../orb_fs
INC := -I$(COMPONENT)/include -I$(FS)/include -I$(FS) -I$(FS)/tests/host
BUILD ?= build

.PHONY: test clean
test: $(BUILD)/test_settings_memory $(BUILD)/test_settings_storage
	$(BUILD)/test_settings_memory
	$(BUILD)/test_settings_storage

$(BUILD)/test_settings_memory: test_settings_memory.c $(COMPONENT)/orb_settings.c $(COMPONENT)/include/orb_settings.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(INC) $(filter %.c,$^) -pthread -o $@

$(BUILD)/fs.o: $(FS)/orb_fs.c $(FS)/include/orb_fs.h $(FS)/orb_fs_port.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(INC) -Drename=test_rename -c $< -o $@

$(BUILD)/test_settings_storage: test_settings_storage.c $(COMPONENT)/orb_settings.c $(COMPONENT)/include/orb_settings.h $(BUILD)/fs.o | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(INC) -DORB_SETTINGS_TEST_STORAGE=1 $(filter %.c %.o,$^) -pthread -o $@

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)
