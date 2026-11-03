# Static builtins provider is reached through orb_apps' optional registration
# hook. Retain it even though no application explicitly calls its symbol.
idf_component_register(SRCS "orb_games.c" "orb_game_logic.c" "orb_game_motion.c" "orb_game_record.c" "orb_game_view.c" "orb_game_sprite_draw.c" "orb_game_chrome.c" "orb_game_native.c" "orb_game_pop.c" "orb_game_beads_draw.c" "orb_game_orbit.c" "orb_game_ring.c" "orb_game_play.c" "orb_game_interpolate.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common lvgl orb_ui orb_fs orb_sound
                       PRIV_REQUIRES orb_tokens freertos heap log esp_timer esp_system orb_display orb_apps orb_memory
                       WHOLE_ARCHIVE)
