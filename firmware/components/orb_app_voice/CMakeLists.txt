idf_component_register(SRCS "voice_flow.c" "voice_text.c" "voice_view.c" "voice_screen.c" "voice_manifest.c" "spoken_reply_screen.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl orb_screen orb_model orb_receipts orb_fonts orb_ui orb_memory)
