#include "orb_nav_admission.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

int main(void)
{
    const char *names[] = {"none", "busy", "drain", "workspace_hold", "source_capture",
        "target_capture", "compositor_begin"};
    assert(orb_nav_admission_last().sequence == 0);
    for (unsigned reason = 1; reason < sizeof(names) / sizeof(*names); reason++) {
        orb_nav_admission_record((orb_nav_admission_reason_t)reason, 100000 - reason, 50000 - reason);
        orb_nav_admission_receipt_t receipt = orb_nav_admission_last();
        assert(receipt.sequence == reason && receipt.reason == (orb_nav_admission_reason_t)reason);
        assert(receipt.psram_free == 100000 - reason && receipt.psram_largest == 50000 - reason);
        assert(!strcmp(orb_nav_admission_name(receipt.reason), names[reason]));
        /* Readback and unrelated restoration do not consume a failure. */
        assert(orb_nav_admission_last().sequence == receipt.sequence);
    }
    assert(!strcmp(orb_nav_admission_name((orb_nav_admission_reason_t)99), "unknown"));
    return 0;
}
