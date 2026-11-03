idf_component_register(SRCS "main.c" "orb_boot.c" "orb_local_services.c" "orb_service_projection.c" "orb_power_services.c" "orb_input_service.c" "orb_panel_policy.c" "orb_light_sleep.c"
                       PRIV_REQUIRES esp_system freertos heap log orb_board orb_display orb_ui orb_shell orb_model orb_home_data orb_lv_simd
                       orb_net orb_link orb_mem orb_voice orb_ota orb_devtools orb_fonts orb_fs orb_apps orb_games orb_app_play orb_nav orb_buttons orb_power orb_time orb_time_app orb_settings orb_sound
                       nvs_flash esp_event esp_app_format esp_timer esp_pm esp_hw_support esp_driver_gpio esp_hal_gpio orb_screen)
