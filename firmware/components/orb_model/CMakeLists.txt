idf_component_register(SRCS "orb_model.c" "orb_model_voice_notify.c" INCLUDE_DIRS "include"
                       REQUIRES orb_fonts
                       PRIV_REQUIRES freertos heap)
