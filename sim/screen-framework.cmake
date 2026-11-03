# Additive phase-3 contract tests. Existing presenters keep their original
# linkage and parity executable; this target uses the real memory admission.
add_library(orb_screen STATIC
    ${COMPONENTS}/orb_screen/orb_screen_host.c
    ${COMPONENTS}/orb_screen/orb_screen_registry.c
    ${COMPONENTS}/orb_screen/orb_context_store.c
    ${COMPONENTS}/orb_screen/orb_router.c
    ${COMPONENTS}/orb_screen/orb_route_context.c
    ${COMPONENTS}/orb_screen/orb_page.c
    ${COMPONENTS}/orb_screen/orb_rim_owner.c
    ${COMPONENTS}/orb_screen/orb_feedback_policy.c
    ${COMPONENTS}/orb_screen/orb_screen_resources.c
    ${COMPONENTS}/orb_nav/orb_nav.c)
target_include_directories(orb_screen PUBLIC
    ${COMPONENTS}/orb_screen/include ${COMPONENTS}/orb_nav/include)
target_link_libraries(orb_screen PUBLIC lvgl orb_memory m)
target_compile_options(orb_screen PRIVATE ${ORB_WARNINGS})

add_executable(test_screen_lifecycle tests/test_screen_lifecycle.c
    tests/orb_screen_lifecycle.c tests/test_screen_delete.c emu_memory.c)
target_include_directories(test_screen_lifecycle PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(test_screen_lifecycle PRIVATE orb_screen)
target_compile_options(test_screen_lifecycle PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME screen_lifecycle COMMAND test_screen_lifecycle)

add_test(NAME guardrails COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/guardrails.py)
add_test(NAME guardrails_bad_samples COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_guardrails.py)

add_executable(test_screen_page tests/test_screen_page.c emu_memory.c)
target_link_libraries(test_screen_page PRIVATE orb_screen)
target_compile_options(test_screen_page PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME screen_page COMMAND test_screen_page)

add_executable(test_route_context tests/test_route_context.c)
target_link_libraries(test_route_context PRIVATE orb_screen)
target_compile_options(test_route_context PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME route_context COMMAND test_route_context)
