CC ?= clang
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined

test: test_rim
	./test_rim

test_rim: test_rim.c ../../orb_rim_math.c ../../orb_rim_feedback.c ../../include/orb_rim_math.h
	$(CC) $(CFLAGS) -I../../include -I../../../orb_sound/include ../../orb_rim_math.c ../../orb_rim_feedback.c test_rim.c -lm -o $@

clean:
	rm -rf test_rim *.dSYM

.PHONY: test clean
