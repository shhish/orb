# Optional orb_apps hooks must remain linked after the service/view split.
idf_component_register(SRCS "orb_time.c" "orb_time_view.c" "orb_time_digits.c" "orb_time_sheet.c"
                            "orb_time_alert.c" "orb_time_command.c" "orb_time_transport.c"
                       INCLUDE_DIRS "include"
                       REQUIRES lvgl orb_ui orb_rim orb_apps orb_sound orb_memory orb_time
                       WHOLE_ARCHIVE
                       PRIV_REQUIRES orb_tokens)
