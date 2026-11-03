# Host tests for orb_net's plain logic, built with the Mac's clang.
#   make         build and run
#   make clean
CC = clang
# stub/ has the one ESP-IDF type orb_net.h needs.
CFLAGS += -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
          -I../.. -I../../include -Istub
LDFLAGS += -fsanitize=address,undefined

TESTS := build/test_orb_net_util build/test_orb_net_sm build/test_orb_net_power build/test_orb_net_radio

all: run

build/test_orb_net_util: test_orb_net_util.c ../../orb_net_util.c ../../orb_net_tz.c ../../orb_net_util.h check.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(filter %.c,$^) $(LDFLAGS) -o $@

build/test_orb_net_sm: test_orb_net_sm.c ../../orb_net_sm.c ../../orb_net_sm.h ../../include/orb_net.h check.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(filter %.c,$^) $(LDFLAGS) -o $@

build/test_orb_net_power: test_orb_net_power.c ../../orb_net_power.c ../../orb_net_util.c ../../orb_net_tz.c ../../orb_net_util.h ../../orb_net_priv.h ../../include/orb_net.h check.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(filter %.c,$^) $(LDFLAGS) -o $@

build/test_orb_net_radio: test_orb_net_radio.c ../../orb_net_util.c ../../orb_net_tz.c ../../orb_net_util.h check.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(filter %.c,$^) $(LDFLAGS) -o $@

run: $(TESTS)
	@for t in $(TESTS); do echo "$$t"; ./$$t || exit 1; done

radio: build/test_orb_net_radio build/test_orb_net_sm build/test_orb_net_power
	@for t in $^; do echo "$$t"; ./$$t || exit 1; done

clean:
	rm -rf build

.PHONY: all run radio clean
