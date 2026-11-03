CC := clang
CFLAGS := -std=c11 -D_POSIX_C_SOURCE=200809L -DORB_TIME_TEST_STORAGE=1 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer
WARN := -Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion
TIME := ../../../orb_time
INC := -I../../../orb_tokens/include -I$(TIME)/include -I../../../orb_apps/include -I../../../orb_sound/include -I../../../orb_fs/include -I../../../orb_fs/tests/host
.PHONY: test clean
test: build/test_time
	TZ=UTC0 ./build/test_time
build/test_time: test_time.c $(TIME)/orb_time.c $(TIME)/orb_time_clock.c $(TIME)/orb_time_record.c ../../orb_time.c ../../../orb_apps/orb_apps.c ../../../orb_apps/orb_marble.c $(TIME)/include/orb_time.h $(TIME)/orb_time_internal.h | build
	$(CC) $(CFLAGS) $(WARN) $(INC) $(filter %.c,$^) -lm -pthread -o $@
build:
	mkdir -p $@
clean:
	rm -rf build
