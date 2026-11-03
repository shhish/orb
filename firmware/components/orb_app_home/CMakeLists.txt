idf_component_register(
    SRCS "home_screen.c" "home_context.c" "home_command.c" "home_manifest.c"
    INCLUDE_DIRS "include"
    REQUIRES orb_screen orb_home orb_home_data lvgl
    PRIV_REQUIRES orb_ui
)
