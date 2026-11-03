idf_component_register(SRCS "orb_apps.c" "orb_marble.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common orb_fs
                       PRIV_REQUIRES orb_tokens)
