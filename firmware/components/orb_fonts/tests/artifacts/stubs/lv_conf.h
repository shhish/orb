#include "../../host/lv_conf.h"
/* No renderer executes in this verifier; its declarations need a valid host
 * config even while the verifier consumes the real firmware sdkconfig. */
#define LV_LOG_PRINTF 0
