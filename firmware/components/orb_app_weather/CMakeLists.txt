idf_component_register(
    SRCS "weather_screen.c"
    INCLUDE_DIRS "include"
    REQUIRES orb_model orb_screen orb_ui orb_fonts lvgl
)
