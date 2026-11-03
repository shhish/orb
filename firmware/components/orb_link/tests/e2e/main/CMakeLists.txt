idf_component_register(SRCS "e2e_main.c" "linux_shims.c"
                       PRIV_REQUIRES orb_link esp_event esp_timer mbedtls nvs_flash)
