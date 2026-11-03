idf_component_register(
    SRCS "face_screen.c" "face_projection.c" "face_manifest.c"
    INCLUDE_DIRS "include"
    REQUIRES orb_screen orb_faces orb_home_data orb_model lvgl
    PRIV_REQUIRES orb_ui orb_time
)
