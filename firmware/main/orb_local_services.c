#include "orb_app_play.h"
#include "orb_local_services.h"
#include "orb_system_state.h"
#include "orb_settings.h"
#include "orb_time.h"
#include "orb_fonts.h"
#include "orb_sound.h"

void orb_local_services_init(void)
{
    (void)orb_app_play_init();
    orb_system_state_init();
    orb_settings_init();
    orb_time_init();
    orb_sound_preferences_init();
    orb_fonts_service_init();
}

void orb_local_services_poll(void)
{
    orb_system_state_service();
    orb_settings_service();
    orb_time_persist_service();
}
