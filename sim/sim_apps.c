/* The real app registry/controller runs in the simulator. No filesystem is
 * mounted and no synthetic score is persisted; storage games fail explicitly. */
#include "orb_fs.h"

#include <string.h>

bool orb_fs_valid_app_id(const char *id)
{
    if (!id || !*id || strlen(id) > ORB_FS_APP_ID_MAX) return false;
    for (const char *p = id; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_'))
            return false;
    }
    return true;
}

bool orb_fs_ready(void) { return false; }
esp_err_t orb_fs_app_prepare(const char *id)
{
    (void)id;
    return ESP_ERR_NOT_SUPPORTED;
}
