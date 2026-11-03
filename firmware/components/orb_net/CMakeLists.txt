idf_component_register(SRCS "orb_net.c" "orb_net_mdns.c" "orb_net_power.c" "orb_net_sm.c" "orb_net_time.c" "orb_net_util.c" "orb_net_tz.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES esp_hw_support esp_system freertos heap log esp_wifi esp_netif esp_event esp_timer nvs_flash bt bootloader_support orb_mem network_provisioning mdns)
