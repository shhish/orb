/* Exercise the installed device LittleFS core and allocator configuration.
 * The flash model is host memory; only production lfs_malloc calls are counted. */
#include "littlefs/lfs.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#if !defined(CONFIG_LITTLEFS_MALLOC_STRATEGY_SPIRAM)
#error "The built firmware must select strict PSRAM allocation for LittleFS"
#endif

static unsigned s_checks, s_calls;
static unsigned s_fail_call;
static size_t s_bytes, s_peak;
static struct { void *pointer; size_t size; } s_allocations[8];
static uint8_t *s_flash;
#define CHECK(value) do { ++s_checks; assert(value); } while (0)

void *heap_caps_malloc(size_t size, uint32_t caps)
{
    CHECK(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ++s_calls;
    if (s_calls == s_fail_call) return NULL;
    for (size_t i = 0; i < sizeof(s_allocations) / sizeof(s_allocations[0]); ++i) {
        if (s_allocations[i].pointer) continue;
        void *pointer = malloc(size);
        CHECK(pointer);
        s_allocations[i].pointer = pointer;
        s_allocations[i].size = size;
        s_bytes += size;
        if (s_bytes > s_peak) s_peak = s_bytes;
        return pointer;
    }
    CHECK(false);
    return NULL;
}

void orb_test_free(void *pointer)
{
    if (!pointer) return;
    for (size_t i = 0; i < sizeof(s_allocations) / sizeof(s_allocations[0]); ++i) {
        if (s_allocations[i].pointer != pointer) continue;
        s_bytes -= s_allocations[i].size;
        s_allocations[i].pointer = NULL;
        free(pointer);
        return;
    }
    CHECK(false);
}

static size_t flash_offset(const struct lfs_config *config, lfs_block_t block,
                            lfs_off_t offset, lfs_size_t size)
{
    CHECK(block < config->block_count && offset <= config->block_size &&
          size <= config->block_size - offset);
    return (size_t)block * config->block_size + offset;
}

static int read_flash(const struct lfs_config *config, lfs_block_t block,
                      lfs_off_t offset, void *data, lfs_size_t size)
{
    memcpy(data, s_flash + flash_offset(config, block, offset, size), size);
    return 0;
}

static int program_flash(const struct lfs_config *config, lfs_block_t block,
                         lfs_off_t offset, const void *data, lfs_size_t size)
{
    uint8_t *target = s_flash + flash_offset(config, block, offset, size);
    const uint8_t *source = data;
    for (lfs_size_t i = 0; i < size; ++i) {
        CHECK((target[i] & source[i]) == source[i]);
        target[i] &= source[i];
    }
    return 0;
}

static int erase_flash(const struct lfs_config *config, lfs_block_t block)
{
    memset(s_flash + flash_offset(config, block, 0, config->block_size),
           0xff, config->block_size);
    return 0;
}

static int sync_flash(const struct lfs_config *config) { (void)config; return 0; }

static uint32_t rd32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
           (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static uint32_t storage_size(const char *path)
{
    FILE *table = fopen(path, "rb");
    CHECK(table);
    uint8_t entry[32];
    uint32_t size = 0;
    while (fread(entry, sizeof(entry), 1, table) == 1) {
        if (entry[0] != 0xaa || entry[1] != 0x50) break;
        if (memcmp(entry + 12, "storage\0", 8)) continue;
        CHECK(entry[2] == 1 && entry[3] == 0x83 && rd32(entry + 4) == 0x1420000);
        size = rd32(entry + 8);
    }
    CHECK(fclose(table) == 0);
    CHECK(size == 0xae0000);
    return size;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    /* Read the exact built table: allocations must not scale with 10.875 MiB. */
    const struct lfs_config config = {
        .read = read_flash, .prog = program_flash, .erase = erase_flash, .sync = sync_flash,
        .read_size = CONFIG_LITTLEFS_READ_SIZE, .prog_size = CONFIG_LITTLEFS_WRITE_SIZE,
        .block_size = 4096, .block_count = storage_size(argv[1]) / 4096,
        .cache_size = CONFIG_LITTLEFS_CACHE_SIZE,
        .lookahead_size = CONFIG_LITTLEFS_LOOKAHEAD_SIZE,
        .block_cycles = CONFIG_LITTLEFS_BLOCK_CYCLES,
    };
    CHECK(config.cache_size == 512 && config.lookahead_size == 128);
    s_flash = malloc((size_t)config.block_size * config.block_count);
    CHECK(s_flash);
    memset(s_flash, 0xff, (size_t)config.block_size * config.block_count);
    lfs_t filesystem = {0};
    CHECK(lfs_mount(&filesystem, &config) == LFS_ERR_CORRUPT);
    CHECK(!s_bytes && s_peak == 1152);
    s_peak = 0;
    CHECK(lfs_format(&filesystem, &config) == 0);
    CHECK(!s_bytes && s_peak == 1152);
    CHECK(lfs_mount(&filesystem, &config) == 0);
    CHECK(s_bytes == 1152 && s_peak == 1152);
    CHECK(lfs_mkdir(&filesystem, "apps") == 0);
    lfs_file_t file;
    CHECK(lfs_file_open(&filesystem, &file, "apps/test", LFS_O_CREAT | LFS_O_WRONLY) == 0);
    CHECK(s_bytes == 1664 && s_peak == 1664);
    const char contents[] = "atomic app state";
    CHECK(lfs_file_write(&filesystem, &file, contents, sizeof(contents)) == sizeof(contents));
    CHECK(lfs_file_sync(&filesystem, &file) == 0);
    CHECK(lfs_file_close(&filesystem, &file) == 0 && s_bytes == 1152);
    CHECK(lfs_unmount(&filesystem) == 0 && !s_bytes);
    for (unsigned failure = 1; failure <= 3; ++failure) {
        s_calls = 0;
        s_fail_call = failure;
        /* The production wrapper calloc's a new context for each retry. */
        memset(&filesystem, 0, sizeof(filesystem));
        CHECK(lfs_mount(&filesystem, &config) == LFS_ERR_NOMEM);
        CHECK(!s_bytes);
        s_calls = 0;
        memset(&filesystem, 0, sizeof(filesystem));
        CHECK(lfs_format(&filesystem, &config) == LFS_ERR_NOMEM);
        CHECK(!s_bytes);
    }
    s_fail_call = 0;
    memset(&filesystem, 0, sizeof(filesystem));
    CHECK(lfs_mount(&filesystem, &config) == 0);
    CHECK(lfs_file_open(&filesystem, &file, "apps/test", LFS_O_RDONLY) == 0);
    char recovered[sizeof(contents)];
    CHECK(lfs_file_read(&filesystem, &file, recovered, sizeof(recovered)) == sizeof(contents));
    CHECK(!memcmp(recovered, contents, sizeof(contents)));
    CHECK(lfs_file_close(&filesystem, &file) == 0);
    CHECK(lfs_unmount(&filesystem) == 0 && !s_bytes);
    free(s_flash);
    printf("LittleFS actual core/config: %u checks; format/mount 1152 B PSRAM, open file +512 B; no internal fallback\n", s_checks);
    return 0;
}
