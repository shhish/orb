idf_component_register(SRCS "orb_voice.c" "voice_core.c" "speech_core.c" "mic_dsp.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES cjson freertos heap log orb_board orb_link orb_mem orb_model orb_sound orb_time esp_timer)
