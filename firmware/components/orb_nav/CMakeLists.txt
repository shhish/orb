idf_component_register(SRCS "orb_nav.c" "orb_nav_compositor.c" "orb_nav_compositor_slots.c" "orb_nav_admission.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES heap orb_memory orb_lv_simd)
