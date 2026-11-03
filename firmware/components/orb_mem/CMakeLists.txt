idf_component_register(SRCS "orb_mem.c" INCLUDE_DIRS "include"
                       REQUIRES heap PRIV_REQUIRES esp_common freertos)
