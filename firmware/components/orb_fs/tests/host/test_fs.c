#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "orb_fs.h"
#include "orb_fs_port.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char s_base[] = "/tmp/orb-fs-test-XXXXXX";
static size_t s_total = 4u * 1024u * 1024u;
static size_t s_used;
static int s_write_mode, s_sync_error, s_rename_error;
static unsigned s_checks;
#define CHECK(value) do { ++s_checks; assert(value); } while (0)

const char *orb_fs_port_base(void) { return s_base; }
esp_err_t orb_fs_port_mount(void) { return ESP_OK; }
void orb_fs_port_lock(void) { }
void orb_fs_port_unlock(void) { }
esp_err_t orb_fs_port_usage(size_t *total, size_t *used) { *total = s_total; *used = s_used; return ESP_OK; }

ssize_t test_write(int fd, const void *data, size_t size)
{
    if (s_write_mode == 1) { errno = ENOSPC; return -1; }
    if (s_write_mode == 2) { s_write_mode = 0; errno = EINTR; return -1; }
    if (s_write_mode == 3 && size > 3) size = 3;
    return write(fd, data, size);
}
int test_fsync(int fd) { if (s_sync_error) { errno = EIO; return -1; } return fsync(fd); }
int test_rename(const char *src, const char *dst)
{
    if (s_rename_error) { errno = EIO; return -1; }
    return rename(src, dst);
}

static void expect(const char *id, const char *key, const char *value)
{
    char data[128];
    size_t size;
    CHECK(orb_fs_read(id, key, data, sizeof(data), &size) == ESP_OK);
    CHECK(size == strlen(value));
    CHECK(!memcmp(data, value, size));
}

int main(void)
{
    CHECK(!orb_fs_ready());
    CHECK(orb_fs_app_prepare("marble") == ESP_ERR_INVALID_STATE);
    CHECK(mkdtemp(s_base));
    CHECK(orb_fs_mount() == ESP_OK);
    CHECK(orb_fs_mount() == ESP_OK);
    CHECK(orb_fs_ready());
    const char *bad[] = { "", "..", "../escape", "x/y", "UPPER", ".pending", "a.b", "with space", "abcdefghijklmnopqrstuvwxyz" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        CHECK(!orb_fs_valid_app_id(bad[i]));
        CHECK(orb_fs_app_prepare(bad[i]) == ESP_ERR_INVALID_ARG);
        if (strlen(bad[i]) <= ORB_FS_APP_ID_MAX)
            CHECK(orb_fs_write("marble", bad[i], "x", 1) == ESP_ERR_INVALID_ARG);
    }
    CHECK(orb_fs_app_prepare(NULL) == ESP_ERR_INVALID_ARG);
    CHECK(orb_fs_write("marble", "abcdefghijklmnopqrstuvwxyz0123456789", "x", 1) == ESP_ERR_INVALID_ARG);
    CHECK(orb_fs_write("marble", "state", NULL, 1) == ESP_ERR_INVALID_ARG);
    CHECK(orb_fs_write("marble", "state", "x", ORB_FS_FILE_LIMIT + 1) == ESP_ERR_INVALID_SIZE);
    CHECK(orb_fs_write("marble", "state", "original", 8) == ESP_OK);
    expect("marble", "state", "original");
    CHECK(orb_fs_write("other", "state", "separate", 8) == ESP_OK);
    expect("marble", "state", "original");
    size_t size = 9;
    char tiny[2];
    CHECK(orb_fs_read("marble", "state", tiny, sizeof(tiny), &size) == ESP_ERR_INVALID_SIZE);
    CHECK(size == 0);
    CHECK(orb_fs_read("marble", "missing", tiny, sizeof(tiny), &size) == ESP_ERR_NOT_FOUND);
    CHECK(orb_fs_read("marble", "state", tiny, sizeof(tiny), NULL) == ESP_ERR_INVALID_ARG);
    s_write_mode = 1;
    CHECK(orb_fs_write("marble", "state", "broken", 6) == ESP_FAIL);
    s_write_mode = 0;
    expect("marble", "state", "original");
    s_sync_error = 1;
    CHECK(orb_fs_write("marble", "state", "unsynced", 8) == ESP_FAIL);
    s_sync_error = 0;
    expect("marble", "state", "original");
    s_rename_error = 1;
    CHECK(orb_fs_write("marble", "state", "uncommitted", 11) == ESP_FAIL);
    s_rename_error = 0;
    expect("marble", "state", "original");
    char pending[128];
    snprintf(pending, sizeof(pending), "%s/apps/marble/.pending", s_base);
    int fd = open(pending, O_CREAT | O_WRONLY, 0600);
    CHECK(fd >= 0);
    CHECK(write(fd, "partial", 7) == 7);
    CHECK(close(fd) == 0);
    expect("marble", "state", "original");
    s_write_mode = 2;
    CHECK(orb_fs_write("marble", "state", "retry", 5) == ESP_OK);
    expect("marble", "state", "retry");
    s_write_mode = 3;
    CHECK(orb_fs_write("marble", "state", "short writes", 12) == ESP_OK);
    s_write_mode = 0;
    expect("marble", "state", "short writes");
    CHECK(access(pending, F_OK) == -1);
    s_used = s_total - ORB_FS_RESERVE;
    CHECK(orb_fs_write("marble", "state", "x", 1) == ESP_ERR_NO_MEM);
    s_used = s_total + 1;
    CHECK(orb_fs_write("marble", "state", "x", 1) == ESP_ERR_NO_MEM);
    s_used = 0;
    static char payload[ORB_FS_FILE_LIMIT];
    for (unsigned i = 0; i < 4; ++i) {
        char key[16]; snprintf(key, sizeof(key), "large%u", i);
        CHECK(orb_fs_write("quota", key, payload, sizeof(payload)) == ESP_OK);
    }
    CHECK(orb_fs_write("quota", "overflow", "x", 1) == ESP_ERR_NO_MEM);
    CHECK(orb_fs_write("quota", "large0", "x", 1) == ESP_OK);
    for (unsigned i = 0; i < ORB_FS_MAX_FILES; ++i) {
        char key[16]; snprintf(key, sizeof(key), "file%u", i);
        CHECK(orb_fs_write("many", key, "", 0) == ESP_OK);
    }
    CHECK(orb_fs_write("many", "overflow", "", 0) == ESP_ERR_NO_MEM);
    for (unsigned i = 0; i < 4; ++i) {
        char id[16]; snprintf(id, sizeof(id), "extra%u", i);
        CHECK(orb_fs_app_prepare(id) == ESP_OK);
    }
    CHECK(orb_fs_app_prepare("ninth") == ESP_ERR_NO_MEM);
    CHECK(orb_fs_app_prepare("marble") == ESP_OK);
    CHECK(orb_fs_remove("marble", "state") == ESP_OK);
    CHECK(orb_fs_remove("marble", "state") == ESP_ERR_NOT_FOUND);
    CHECK(orb_fs_remove("marble", "../x") == ESP_ERR_INVALID_ARG);
    CHECK(orb_fs_usage(NULL, &size) == ESP_ERR_INVALID_ARG);
    printf("orb_fs: %u checks passed\n", s_checks);
    return 0;
}
