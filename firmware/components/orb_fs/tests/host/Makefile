CC := clang
CFLAGS := -std=c11 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -D_POSIX_C_SOURCE=200809L
WARN := -Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion
INC := -I. -I../../include -I../..
DEVICE_CONFIG := ../../../../build/config
LITTLEFS := ../../../../managed_components/joltwallet__littlefs/src
LFS_INC := -I$(DEVICE_CONFIG) -Istubs -I$(LITTLEFS)
.PHONY: test clean
test: build/test_fs build/test_mount build/test_littlefs_memory
	./build/test_fs
	./build/test_mount
	./build/test_littlefs_memory ../../../../build/partition_table/partition-table.bin
build/fs.o: ../../orb_fs.c ../../include/orb_fs.h | build
	$(CC) $(CFLAGS) $(WARN) $(INC) -Dwrite=test_write -Dfsync=test_fsync -Drename=test_rename -c $< -o $@
build/test_fs: test_fs.c build/fs.o | build
	$(CC) $(CFLAGS) $(WARN) $(INC) $^ -o $@
build/test_mount: test_mount.c ../../orb_fs_port_esp.c $(DEVICE_CONFIG)/sdkconfig.h | build
	$(CC) $(CFLAGS) $(WARN) $(INC) -I$(DEVICE_CONFIG) -Istubs $(filter %.c,$^) -o $@
build/lfs.o: $(LITTLEFS)/littlefs/lfs.c $(LITTLEFS)/lfs_config.h $(DEVICE_CONFIG)/sdkconfig.h | build
	$(CC) $(CFLAGS) -Wall -Wextra -Werror $(LFS_INC) -DLFS_CONFIG=lfs_config.h -Dfree=orb_test_free -c $< -o $@
build/lfs_config.o: $(LITTLEFS)/lfs_config.c $(LITTLEFS)/lfs_config.h $(DEVICE_CONFIG)/sdkconfig.h | build
	$(CC) $(CFLAGS) -Wall -Wextra -Werror $(LFS_INC) -DLFS_CONFIG=lfs_config.h -Dfree=orb_test_free -c $< -o $@
build/test_littlefs_memory: test_littlefs_memory.c build/lfs.o build/lfs_config.o | build
	$(CC) $(CFLAGS) $(WARN) $(LFS_INC) -DLFS_CONFIG=lfs_config.h $^ -o $@
build:
	mkdir -p $@
clean:
	rm -rf build
