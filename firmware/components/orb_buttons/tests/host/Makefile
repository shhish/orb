CC := clang
CFLAGS := -std=c11 -g -O1 -Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion -fsanitize=address,undefined -fno-omit-frame-pointer
.PHONY: test clean
test: build/test_buttons
	./build/test_buttons
build/test_buttons: test_buttons.c ../../orb_buttons.c ../../include/orb_buttons.h
	mkdir -p build
	$(CC) $(CFLAGS) -I../../include test_buttons.c ../../orb_buttons.c -o $@
clean:
	rm -rf build
