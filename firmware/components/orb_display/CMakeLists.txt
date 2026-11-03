idf_component_register(SRCS "orb_display.c" "orb_display_touch.c" "orb_display_profile.c" "orb_display_geom.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common lvgl
                       PRIV_REQUIRES esp_hw_support esp_rom freertos heap log orb_board orb_mem esp_driver_gpio esp_timer esp_lcd_touch esp_pm)
