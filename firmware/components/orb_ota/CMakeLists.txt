idf_component_register(SRCS "orb_ota.c" "orb_ota_download.c" "orb_ota_logic.c" "orb_ota_assets.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES esp_system freertos heap log orb_link orb_mem orb_fonts app_update bootloader_support esp_https_ota esp_http_client esp_partition
                                     esp_timer esp_app_format nvs_flash mbedtls cjson)
