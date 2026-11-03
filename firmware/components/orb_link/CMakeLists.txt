idf_component_register(SRCS "orb_link.c" "link_proto.c" "link_sas.c" "link_backoff.c" "link_v11.c" "link_memory.c" "link_home.c" "link_device.c" "link_radio.c" "link_crash.c" "link_crash_elf.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common orb_model cjson
                       PRIV_REQUIRES espcoredump esp_partition xtensa esp_hw_support esp_system freertos heap log orb_net orb_home_data orb_mem orb_memory orb_time app_update esp_app_format esp_http_client esp_timer mbedtls nvs_flash esp_websocket_client)
