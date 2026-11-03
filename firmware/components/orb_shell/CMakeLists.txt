# Explicit sources also work during IDF's script-mode dependency scan.
set(COMP_FONT_SOURCES
    "fonts/orb_nunito_21_semibold.c" "fonts/orb_nunito_21_bold.c"
    "fonts/orb_nunito_25_semibold.c" "fonts/orb_nunito_25_bold.c"
    "fonts/orb_nunito_28_semibold.c" "fonts/orb_nunito_28_bold.c")

idf_component_register(SRCS "orb_shell.c" "orb_shell_power.c" "orb_workspace.c" "orb_workspace_snapshot.c" "orb_neighbour.c" "system/charging_screen.c" "system/mac_screen.c" "system/ota_screen.c" "system/boot_screen.c" "system/system_scaffold.c" "control_screen.c" "apps_screen.c" "stack_screen.c" "system/onboarding_screen.c" "system/updates_screen.c" "system/system_views.c" "orb_settings_host.c" "orb_settings_projection.c" "orb_weather_host.c" "orb_agents_host.c" "orb_voice_host.c" "orb_notifications_host.c" "orb_app_host.c" "orb_home_host.c" "orb_time_host.c" "orb_play_host.c" "orb_faces_host.c" "orb_stack.c" "orb_scroll_fixture.c" "orb_feedback.c" "orb_shell_services.c" "orb_sound_overlay.c" "orb_system_state.c" "orb_system_copy.c" "orb_place_copy.c" "orb_place_view.c" ${COMP_FONT_SOURCES}
                       INCLUDE_DIRS "include"
                       REQUIRES lvgl orb_ui orb_screen orb_app_settings orb_app_weather orb_app_home orb_app_time orb_app_faces orb_app_play orb_app_agents orb_app_voice orb_app_notifications orb_receipts orb_fonts orb_status orb_model orb_faces orb_home orb_home_data orb_apps orb_games orb_sound orb_nav orb_rim orb_time_app orb_time orb_buttons orb_power orb_board orb_fs
                       PRIV_REQUIRES orb_tokens freertos heap orb_display orb_memory esp_timer)
