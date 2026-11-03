idf_component_register(
    SRCS "time_screen.c" "time_command.c" "time_manifest.c"
    INCLUDE_DIRS "include"
    REQUIRES orb_screen orb_time_app lvgl
    PRIV_REQUIRES orb_ui
)
