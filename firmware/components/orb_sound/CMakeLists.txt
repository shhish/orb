idf_component_register(SRCS "orb_sound.c" "orb_sound_preferences.c" "orb_sound_core.c" "orb_sound_pcm.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_codec_dev log orb_board orb_fonts esp_timer nvs_flash freertos heap esp_hw_support)
