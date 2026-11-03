#include "orb_fs.h"
#include "orb_fs_port.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PATH_SIZE 128
static bool s_ready;

static bool valid_name(const char *name, size_t max)
{
    if (!name || !name[0]) return false;
    size_t n = 0;
    for (; name[n]; ++n) {
        unsigned char c = (unsigned char)name[n];
        if (n >= max || !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;
    }
    return true;
}

bool orb_fs_valid_app_id(const char *id) { return valid_name(id, ORB_FS_APP_ID_MAX); }
bool orb_fs_ready(void) { return s_ready; }

static bool make_path(char *out, const char *id, const char *key)
{
    int n = key ? snprintf(out, PATH_SIZE, "%s/apps/%s/%s", orb_fs_port_base(), id, key) :
                  snprintf(out, PATH_SIZE, "%s/apps/%s", orb_fs_port_base(), id);
    return n >= 0 && n < PATH_SIZE;
}

static esp_err_t make_dir(const char *path)
{
    if (!mkdir(path, 0700)) return ESP_OK;
    struct stat st;
    return errno == EEXIST && !stat(path, &st) && S_ISDIR(st.st_mode) ? ESP_OK : ESP_FAIL;
}

esp_err_t orb_fs_mount(void)
{
    if (s_ready) return ESP_OK;
    esp_err_t err = orb_fs_port_mount();
    if (err != ESP_OK) return err;
    char path[PATH_SIZE];
    int n = snprintf(path, sizeof(path), "%s/apps", orb_fs_port_base());
    if (n < 0 || n >= (int)sizeof(path)) return ESP_ERR_INVALID_SIZE;
    err = make_dir(path);
    if (err == ESP_OK) s_ready = true;
    return err;
}

static esp_err_t prepare_locked(const char *id)
{
    char path[PATH_SIZE];
    if (!make_path(path, id, NULL)) return ESP_ERR_INVALID_SIZE;
    struct stat st;
    if (!stat(path, &st)) return S_ISDIR(st.st_mode) ? ESP_OK : ESP_FAIL;
    if (errno != ENOENT) return ESP_FAIL;
    char parent[PATH_SIZE];
    int n = snprintf(parent, sizeof(parent), "%s/apps", orb_fs_port_base());
    if (n < 0 || n >= (int)sizeof(parent)) return ESP_ERR_INVALID_SIZE;
    DIR *dir = opendir(parent);
    if (!dir) return ESP_FAIL;
    size_t count = 0;
    struct dirent *entry;
    esp_err_t err = ESP_OK;
    for (;;) {
        errno = 0;
        entry = readdir(dir);
        if (!entry) { if (errno) err = ESP_FAIL; break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        ++count;
        if (count >= ORB_FS_MAX_APPS) break;
    }
    if (closedir(dir)) err = ESP_FAIL;
    if (err != ESP_OK) return err;
    if (count >= ORB_FS_MAX_APPS) return ESP_ERR_NO_MEM;
    return make_dir(path);
}

esp_err_t orb_fs_app_prepare(const char *id)
{
    if (!orb_fs_valid_app_id(id)) return ESP_ERR_INVALID_ARG;
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    orb_fs_port_lock();
    esp_err_t err = prepare_locked(id);
    orb_fs_port_unlock();
    return err;
}

static bool valid_request(const char *id, const char *key)
{
    return orb_fs_valid_app_id(id) && valid_name(key, ORB_FS_KEY_MAX);
}

esp_err_t orb_fs_read(const char *id, const char *key, void *data, size_t capacity, size_t *size)
{
    if (size) *size = 0;
    if (!valid_request(id, key) || !size || (!data && capacity)) return ESP_ERR_INVALID_ARG;
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    char path[PATH_SIZE];
    if (!make_path(path, id, key)) return ESP_ERR_INVALID_SIZE;
    orb_fs_port_lock();
    int fd = open(path, O_RDONLY);
    esp_err_t err = ESP_OK;
    if (fd < 0) err = errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    else {
        struct stat st;
        if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || (size_t)st.st_size > ORB_FS_FILE_LIMIT)
            err = ESP_FAIL;
        else if ((size_t)st.st_size > capacity) err = ESP_ERR_INVALID_SIZE;
        else {
            size_t at = 0, length = (size_t)st.st_size;
            while (at < length) {
                ssize_t n = read(fd, (char *)data + at, length - at);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) { err = ESP_FAIL; break; }
                at += (size_t)n;
            }
            if (err == ESP_OK) *size = length;
        }
        if (close(fd) && err == ESP_OK) { err = ESP_FAIL; *size = 0; }
    }
    orb_fs_port_unlock();
    return err;
}

static esp_err_t quota_locked(const char *id, const char *key, size_t size)
{
    char path[PATH_SIZE];
    if (!make_path(path, id, NULL)) return ESP_ERR_INVALID_SIZE;
    DIR *dir = opendir(path);
    if (!dir) return ESP_FAIL;
    size_t count = 0, used = 0;
    bool replacing = false;
    esp_err_t err = ESP_OK;
    struct dirent *entry;
    for (;;) {
        errno = 0;
        entry = readdir(dir);
        if (!entry) { if (errno) err = ESP_FAIL; break; }
        /* One reserved staging file is overwritten on retry after a reset. */
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") || !strcmp(entry->d_name, ".pending"))
            continue;
        if (!valid_name(entry->d_name, ORB_FS_KEY_MAX) || !make_path(path, id, entry->d_name)) {
            err = ESP_FAIL; break;
        }
        struct stat st;
        if (stat(path, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || (size_t)st.st_size > ORB_FS_FILE_LIMIT) {
            err = ESP_FAIL; break;
        }
        ++count;
        if (!strcmp(entry->d_name, key)) replacing = true;
        else used += (size_t)st.st_size;
        if (used > ORB_FS_APP_QUOTA || count > ORB_FS_MAX_FILES) { err = ESP_ERR_NO_MEM; break; }
    }
    if (closedir(dir)) err = ESP_FAIL;
    if (err != ESP_OK) return err;
    if ((!replacing && count >= ORB_FS_MAX_FILES) || size > ORB_FS_APP_QUOTA - used) return ESP_ERR_NO_MEM;
    size_t total, occupied;
    err = orb_fs_port_usage(&total, &occupied);
    if (err != ESP_OK) return err;
    /* Reserve space for metadata/wear levelling, and the entire new file while
     * the previous committed version still exists. Never fill LittleFS. */
    if (occupied > total || total - occupied < ORB_FS_RESERVE || size > total - occupied - ORB_FS_RESERVE)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}

esp_err_t orb_fs_write(const char *id, const char *key, const void *data, size_t size)
{
    if (!valid_request(id, key) || (!data && size)) return ESP_ERR_INVALID_ARG;
    if (size > ORB_FS_FILE_LIMIT) return ESP_ERR_INVALID_SIZE;
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    char path[PATH_SIZE], pending[PATH_SIZE];
    if (!make_path(path, id, key) || !make_path(pending, id, ".pending")) return ESP_ERR_INVALID_SIZE;
    orb_fs_port_lock();
    esp_err_t err = prepare_locked(id);
    if (err == ESP_OK) err = quota_locked(id, key, size);
    if (err == ESP_OK) {
        int fd = open(pending, O_CREAT | O_TRUNC | O_WRONLY, 0600);
        if (fd < 0) err = ESP_FAIL;
        else {
            size_t at = 0;
            while (at < size) {
                ssize_t n = write(fd, (const char *)data + at, size - at);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) { err = ESP_FAIL; break; }
                at += (size_t)n;
            }
            if (err == ESP_OK && fsync(fd)) err = ESP_FAIL;
            if (close(fd) && err == ESP_OK) err = ESP_FAIL;
            if (err == ESP_OK && rename(pending, path)) err = ESP_FAIL;
        }
        if (err != ESP_OK) unlink(pending);
    }
    orb_fs_port_unlock();
    return err;
}

esp_err_t orb_fs_remove(const char *id, const char *key)
{
    if (!valid_request(id, key)) return ESP_ERR_INVALID_ARG;
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    char path[PATH_SIZE];
    if (!make_path(path, id, key)) return ESP_ERR_INVALID_SIZE;
    orb_fs_port_lock();
    esp_err_t err = unlink(path) ? (errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL) : ESP_OK;
    orb_fs_port_unlock();
    return err;
}

esp_err_t orb_fs_usage(size_t *total, size_t *used)
{
    if (!total || !used) return ESP_ERR_INVALID_ARG;
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    orb_fs_port_lock();
    esp_err_t err = orb_fs_port_usage(total, used);
    orb_fs_port_unlock();
    return err;
}
