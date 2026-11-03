idf_component_register(SRCS "orb_faces.c" "orb_faces_motion.c" "orb_faces_data.c" "orb_faces_model.c" "orb_faces_measure.c" INCLUDE_DIRS "include"
                       REQUIRES lvgl orb_fonts orb_ui orb_model orb_memory orb_settings orb_sound
                       WHOLE_ARCHIVE)
