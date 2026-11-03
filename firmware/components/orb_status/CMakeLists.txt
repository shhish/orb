idf_component_register(SRCS "src/orb_status.c"
                       INCLUDE_DIRS "include"
                       REQUIRES lvgl orb_fonts
                       PRIV_REQUIRES orb_ui)
