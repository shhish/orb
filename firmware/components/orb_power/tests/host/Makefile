CC := clang
CFLAGS := -std=c11 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer
WARN := -Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion
INC := -I../../include -Istubs
SRC := ../../orb_power.c ../../orb_power_imu.c ../../orb_power_board.c
.PHONY: test clean
test: build/test_power build/test_imu_traces build/test_policy
	./build/test_power
	./build/test_imu_traces
	./build/test_policy
build/test_power: test_power.c $(SRC) ../../include/orb_power.h ../../include/orb_power_board.h ../../orb_power_private.h | build
	$(CC) $(CFLAGS) $(WARN) $(INC) test_power.c $(SRC) -lm -o $@
build/test_imu_traces: test_imu_traces.c ../../orb_power.c ../../orb_power_imu.c ../../include/orb_power.h ../../orb_power_private.h | build
	$(CC) $(CFLAGS) $(WARN) $(INC) test_imu_traces.c ../../orb_power.c ../../orb_power_imu.c -lm -o $@
build/test_policy: test_policy.c ../../orb_power.c ../../orb_power_imu.c ../../include/orb_power.h ../../orb_power_private.h | build
	$(CC) $(CFLAGS) $(WARN) $(INC) test_policy.c ../../orb_power.c ../../orb_power_imu.c -lm -o $@
build:
	mkdir -p $@
clean:
	rm -rf build
