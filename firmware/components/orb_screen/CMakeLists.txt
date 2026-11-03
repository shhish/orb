idf_component_register(
    SRCS "orb_screen_host.c" "orb_screen_registry.c" "orb_context_store.c"
         "orb_router.c" "orb_route_context.c" "orb_page.c" "orb_rim_owner.c" "orb_feedback_policy.c" "orb_screen_resources.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl orb_nav orb_memory)
