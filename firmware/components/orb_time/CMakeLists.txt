idf_component_register(SRCS "orb_time.c" "orb_time_clock.c" "orb_time_record.c" "orb_time_voice.c"
                       INCLUDE_DIRS "include"
                       REQUIRES orb_sound
                       PRIV_REQUIRES orb_fs freertos)
