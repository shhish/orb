CC := clang
CFLAGS := -std=gnu11 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer
WARN := -Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion
INC := -I../../orb_tokens/include -I../include -I../../orb_apps/include -I../../orb_fs/include -I../../orb_fs/tests/host
.PHONY: test clean
test: build/test_services
	./build/test_services
build/test_services: test_services.c ../orb_shell_services.c ../../orb_apps/orb_apps.c ../../orb_apps/orb_marble.c ../../../../sim/sim_apps.c | build
	$(CC) $(CFLAGS) $(WARN) $(INC) $^ -pthread -lm -o $@
build:
	mkdir -p $@
clean:
	rm -rf build
