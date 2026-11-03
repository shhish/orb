# Requirements must be unconditional: Kconfig values aren't known yet when
# ESP-IDF first scans them.
set(srcs "")
if(CONFIG_ORB_DEVTOOLS)
    set(srcs "orb_devtools.c" "orb_devtools_power.c" "orb_devtools_crash.c" "orb_devtools_diagnostics.c" "orb_devtools_sender.c" "orb_devtools_round.c" "orb_devtools_heaptrace.c")
endif()

# WHOLE_ARCHIVE: nothing calls into this component; it starts from its
# ESP_SYSTEM_INIT_FN, which the linker would otherwise drop.
idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       REQUIRES lvgl orb_buttons orb_board orb_power
                       PRIV_REQUIRES espcoredump cjson orb_model heap freertos esp_system spi_flash esp_hw_support esp_common xtensa esp_rom esp_hal_usb log orb_link orb_home_data orb_net orb_display orb_demo orb_ui orb_mem orb_memory orb_nav orb_shell orb_fonts orb_apps orb_games orb_sound orb_voice esp_timer esp_pm
                       WHOLE_ARCHIVE)
