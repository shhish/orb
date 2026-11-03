CC := clang
CFLAGS := -std=c11 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer
WARN := -Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion
INC := -I../../../orb_tokens/include -I../../include
.PHONY: test clean
test: build/test_game_logic build/test_game_runtime
	./build/test_game_logic
	./build/test_game_runtime
build/test_game_logic: test_game_logic.c ../../orb_game_logic.c ../../orb_game_motion.c ../../orb_game_record.c ../../include/orb_game_logic.h | build
	$(CC) $(CFLAGS) $(WARN) $(INC) $(filter %.c,$^) -lm -o $@
build:
	mkdir -p $@
clean:
	rm -rf build

RUNTIME_INC := -I../../../orb_tokens/include -Iruntime_stubs -I../../include -I../../../orb_apps/include -I../../../orb_fs/include -I../../../orb_fs/tests/host
build/runtime.o: ../../orb_games.c ../../include/orb_games.h ../../include/orb_game_logic.h ../../include/orb_game_view.h runtime_stubs/lvgl.h runtime_stubs/orb_display.h | build
	$(CC) $(CFLAGS) $(WARN) $(RUNTIME_INC) -Dcalloc=orb_test_calloc -Dfree=orb_test_free -c $< -o $@
build/record_logic.o: ../../orb_game_logic.c ../../include/orb_game_logic.h | build
	$(CC) $(CFLAGS) $(WARN) $(INC) -Dorb_game_id=orb_test_real_id -Dorb_game_init=orb_test_real_init -Dorb_game_step=orb_test_real_step -Dorb_game_action=orb_test_real_action -c $< -o $@
build/test_game_runtime: test_game_runtime.c build/runtime.o build/record_logic.o ../../orb_game_motion.c ../../orb_game_record.c ../../orb_game_interpolate.c ../../../orb_apps/orb_apps.c ../../../orb_apps/orb_marble.c | build
	$(CC) $(CFLAGS) $(WARN) $(RUNTIME_INC) $^ -lm -o $@
