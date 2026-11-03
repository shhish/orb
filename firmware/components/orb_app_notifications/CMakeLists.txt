idf_component_register(SRCS "notifications_view.c" "notifications_screen.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl orb_receipts orb_model orb_ui orb_fonts orb_memory orb_screen)
