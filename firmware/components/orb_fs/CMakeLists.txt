idf_component_register(SRCS "orb_fs.c" "orb_fs_port_esp.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES freertos log vfs littlefs esp_partition heap)
