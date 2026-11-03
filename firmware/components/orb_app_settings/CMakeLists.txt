idf_component_register(
    SRCS "accessibility_screen.c" "about_screen.c" "sound_screen.c" "display_screen.c" "battery_screen.c" "settings_screen.c" "units_screen.c" "settings_list_page.c" "settings_manifest.c" "agent_settings_screen.c"
    INCLUDE_DIRS "include"
    REQUIRES orb_sound orb_time orb_model orb_screen orb_ui orb_fonts lvgl
)
