idf_component_register(
    SRCS "play_screen.c" "game_screen.c" "play_manifest.c"
    INCLUDE_DIRS "include"
    REQUIRES orb_screen orb_games lvgl
    PRIV_REQUIRES orb_ui orb_memory
)
