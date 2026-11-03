idf_component_register(SRCS "board_ws175c.c" "board_panel_power.c" "board_audio.c" "board_buttons.c" "board_usb.c" "axp2101.c" "qmi8658.c" "orb_imu.c" "orb_senses.c" "orb_battery.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common esp_lcd esp_lcd_touch esp_codec_dev esp_lcd_co5300 esp_lcd_touch_cst9217
                       PRIV_REQUIRES esp_hw_support esp_timer freertos log esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_spi nvs_flash)
