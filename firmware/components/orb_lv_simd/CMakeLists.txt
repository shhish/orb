idf_component_register(SRCS "src/orb_lv_blend.c"
                            "src/lv_color_blend_to_rgb565_esp32s3.S"
                            "src/lv_rgb565_blend_normal_to_rgb565_esp32s3.S"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "src"
                       REQUIRES lvgl)

# LVGL's renderer includes orb_lv_blend.h and calls into this component.
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_include_directories(${lvgl_lib} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(${lvgl_lib} PRIVATE ${COMPONENT_LIB})
