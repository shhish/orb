CC := clang
CFLAGS := -std=c11 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer
WARN := -Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion
INC := -I../../../orb_tokens/include -I../../include -I../../../orb_fs/include -I../../../orb_fs/tests/host
.PHONY: test clean
test: build/test_apps
	./build/test_apps
build/test_apps: test_apps.c ../../orb_apps.c ../../orb_marble.c | build
	$(CC) $(CFLAGS) $(WARN) $(INC) $^ -lm -o $@
build:
	mkdir -p $@
clean:
	rm -rf build
