# TEST ONLY: ESP-IDF's app_update has no linux port; orb_link only asks it
# which slot is running.
idf_component_register(SRCS "esp_ota_ops_linux.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_partition)
