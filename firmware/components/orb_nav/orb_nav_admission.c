#include "orb_nav_admission.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

static orb_nav_admission_receipt_t s_receipt;

const char *orb_nav_admission_name(orb_nav_admission_reason_t reason)
{
    static const char *const names[] = {"none", "busy", "drain", "workspace_hold",
        "source_capture", "target_capture", "compositor_begin"};
    return (unsigned)reason < sizeof(names) / sizeof(*names) ? names[reason] : "unknown";
}

void orb_nav_admission_record(orb_nav_admission_reason_t reason, size_t free_bytes, size_t largest)
{
    s_receipt = (orb_nav_admission_receipt_t){s_receipt.sequence + 1, reason, free_bytes, largest};
}

void orb_nav_admission_fail(orb_nav_admission_reason_t reason)
{
#ifdef ESP_PLATFORM
    orb_nav_admission_record(reason, heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
#else
    orb_nav_admission_record(reason, 0, 0);
#endif
}

orb_nav_admission_receipt_t orb_nav_admission_last(void)
{
    return s_receipt;
}
