idf_component_register(SRCS "orb_home_view.c" "orb_home_rooms.c" "orb_home_motion.c" "orb_home_pages.c" "orb_home_detail.c" "orb_home_detail_view.c" "orb_home_fill.c" "orb_home_text.c" "orb_home_payload.c" "orb_home_command.c"
                       INCLUDE_DIRS "include"
                       REQUIRES lvgl orb_ui orb_fonts orb_sound orb_model orb_rim orb_home_data orb_memory
                       PRIV_REQUIRES freertos heap)
