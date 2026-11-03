idf_component_register(SRCS "agents_screen.c" "agents_manifest.c" "agents_input.c"
    "agents_common.c" "agents_page.c" "agents_glance.c" "agents_sessions.c"
    "agents_usage.c" "agents_dot.c" "agents_card.c"
    "orb_agents_copy.c" "orb_session_copy.c" "orb_usage_copy.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl orb_screen orb_model orb_receipts orb_ui orb_fonts orb_time)
