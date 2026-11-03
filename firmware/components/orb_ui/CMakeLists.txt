idf_component_register(SRCS "tokens/orb_palette.c" "orb_ui.c" "orb_scroll_edge.c" "orb_round_source.c" "orb_round_text.c" "orb_input.c" "orb_glass.c" "orb_glass_masks.c" "orb_glass_fused.c" "orb_glass_sdf.c" "orb_motion.c" "orb_motion_math.c"
                            "orb_delight_motion.c" "orb_kit_motion.c" "orb_kit_disc_motion.c" "orb_kit_feedback_motion.c" "orb_kit_value_roll.c" "orb_kit.c" "orb_kit_shell_controls.c" "orb_kit_rim.c" "orb_kit_controls.c" "orb_kit_level.c" "orb_kit_row.c" "orb_kit_row_effects.c" "orb_kit_row_layout.c" "orb_kit_text.c" "orb_kit_value.c" "orb_kit_scroll.c" "orb_kit_status.c" "orb_kit_fit.c" "orb_kit_focus.c" "orb_kit_pages.c" "orb_kit_detail_page.c" "orb_kit_state.c" "orb_kit_receipt.c" "orb_kit_sheet.c" "orb_kit_confirm.c" "orb_kit_page_label.c" "orb_kit_page_dots.c"
                       INCLUDE_DIRS "include"
                       REQUIRES orb_tokens lvgl orb_fonts orb_memory
                       PRIV_REQUIRES heap esp_timer)

if(CONFIG_ORB_RENDER_FUSED_GLASS AND CONFIG_IDF_TARGET_ESP32S3)
    target_sources(${COMPONENT_LIB} PRIVATE "orb_glass_fused_s3.S")
endif()
