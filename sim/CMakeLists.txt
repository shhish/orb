# Orb's desktop simulator (macOS): the firmware's LVGL fork, orb_ui and
# orb_demo as they are, over a fake board. sim/run.sh and sim/shot.sh build it.
cmake_minimum_required(VERSION 3.20)
project(orb_sim C)

# Keep implicit Apple Silicon builds identical on fresh and cached configures.
# A blank architecture request otherwise drops the baseline's explicit -arch
# flag after reconfiguration; an owner-specified architecture still wins.
if(APPLE AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64" AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES arm64)
endif()

# One compile cache across worktrees, so a fresh worktree does not cost a full
# rebuild. Absolute -D values stay in the hash, so a stale path cannot leak over.
find_program(ORB_CCACHE ccache)
if(ORB_CCACHE AND NOT CMAKE_C_COMPILER_LAUNCHER)
    get_filename_component(ORB_WORKTREE "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
    set(CMAKE_C_COMPILER_LAUNCHER
        ${CMAKE_COMMAND} -E env CCACHE_BASEDIR=${ORB_WORKTREE} CCACHE_NOHASHDIR=1 ${ORB_CCACHE})
endif()

# gnu, as ESP-IDF compiles the firmware (M_PI and friends without feature macros)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Instrument the whole simulator, including both software draw workers. Tests
# still free font records/mappings; no quarantine or immortal fake port is used.
option(ORB_SANITIZE_ADDRESS "Instrument simulator and tests with AddressSanitizer" OFF)
if(ORB_SANITIZE_ADDRESS)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

set(FIRMWARE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware)
set(COMPONENTS ${FIRMWARE}/components)
set(LVGL_DIR ${FIRMWARE}/third_party/lvgl__lvgl)

find_package(SDL2 CONFIG REQUIRED)
find_package(ZLIB REQUIRED)

add_library(orb_memory STATIC ${COMPONENTS}/orb_memory/orb_memory.c)
target_include_directories(orb_memory PUBLIC ${COMPONENTS}/orb_memory/include)

# LVGL, configured by lv_conf.h here (the firmware's settings). Its own CMake
# is skipped: it fetches dependencies this doesn't use.
file(GLOB_RECURSE LVGL_SOURCES CONFIGURE_DEPENDS ${LVGL_DIR}/src/*.c)
add_library(lvgl STATIC ${LVGL_SOURCES})
target_include_directories(lvgl SYSTEM PUBLIC ${LVGL_DIR} ${LVGL_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE LV_KCONFIG_IGNORE)
target_link_libraries(lvgl PUBLIC orb_memory)
# Host counterparts of the firmware's independent A/B switches.
option(ORB_RENDER_FUSED_GLASS "Fuse reduced glass material reconstruction" OFF)
option(ORB_RENDER_ROWS_AT_SIZE "Render moving rows from native-size glyph atlases" OFF)
foreach(hotspot FUSED_GLASS ROWS_AT_SIZE)
    if(ORB_RENDER_${hotspot})
        target_compile_definitions(lvgl PUBLIC CONFIG_ORB_RENDER_${hotspot}=1)
    endif()
endforeach()

option(ORB_OPEN_LATENCY_FIXES "Preload/stage app opens (OFF is instrumentation control)" ON)
add_compile_definitions(ORB_OPEN_LATENCY_FIXES=$<BOOL:${ORB_OPEN_LATENCY_FIXES}>)
option(ORB_UI_SCROLL_EDGE_SOFT "Consistent soft list/prose scroll edges" ON)
add_compile_definitions(ORB_UI_SCROLL_EDGE_SOFT=$<BOOL:${ORB_UI_SCROLL_EDGE_SOFT}>)
option(ORB_DISPLAY_GLASS_SINGLE_PASS "Cull off-band glass tasks" OFF)
option(ORB_DISPLAY_GLASS_MOTION_QUALITY "Quarter-resolution moving backdrop" OFF)
option(ORB_DISPLAY_RASTER_SCALE_CACHE "Exact scaled row variants" OFF)
option(ORB_DISPLAY_RGB565_SCALE_FAST "Exact opaque scale-only specialization" OFF)
option(ORB_DISPLAY_RASTER_COVER_CHECK "Correct cached-root cover search" ON)
option(ORB_DISPLAY_DRAW_PROFILE "Collect draw-task timing counters" OFF)
option(ORB_DISPLAY_RENDER_ACCOUNTING "Collect whole-update preparation and cadence counters" OFF)
option(ORB_DISPLAY_RETAINED_MATERIAL "Retain exact settled material presentation" OFF)
option(ORB_DISPLAY_RETAINED_SETTLED_ONLY "Suspend retained material during backdrop motion" OFF)
option(ORB_DISPLAY_GLASS_SOLID_FAST "Exact uniform-backdrop blur bypass" ON)
option(ORB_DISPLAY_GLASS_SPARSE_MASKS "Skip transparent mask spans" OFF)
option(ORB_DISPLAY_TE_PACING "TE deadline scheduling (host planner tests only)" OFF)
foreach(experiment GLASS_SINGLE_PASS GLASS_MOTION_QUALITY RASTER_SCALE_CACHE RGB565_SCALE_FAST RASTER_COVER_CHECK DRAW_PROFILE RENDER_ACCOUNTING RETAINED_MATERIAL RETAINED_SETTLED_ONLY GLASS_SOLID_FAST GLASS_SPARSE_MASKS TE_PACING)
    if(ORB_DISPLAY_${experiment})
        target_compile_definitions(lvgl PUBLIC CONFIG_ORB_DISPLAY_${experiment}=1)
    endif()
endforeach()
# Third-party code: its warnings are LVGL's to fix, and would bury ours.
target_compile_options(lvgl PRIVATE -w)

# Orb's code, warned about as ESP-IDF does, but every warning an error.
set(ORB_WARNINGS -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Werror)

file(GLOB FONT_SOURCES CONFIGURE_DEPENDS
    ${COMPONENTS}/orb_fonts/src/*.c ${COMPONENTS}/orb_fonts/fonts/*.c)
file(GLOB COMPLICATION_FONTS CONFIGURE_DEPENDS
    ${COMPONENTS}/orb_shell/fonts/*.c)
add_library(orb_tokens INTERFACE)
target_include_directories(orb_tokens INTERFACE ${COMPONENTS}/orb_tokens/include)

add_library(orb STATIC
    ${FONT_SOURCES}
    ${COMPONENTS}/orb_buttons/orb_buttons.c
    ${COMPONENTS}/orb_nav/orb_nav.c
    ${COMPONENTS}/orb_nav/orb_nav_compositor.c
    ${COMPONENTS}/orb_nav/orb_nav_compositor_slots.c
    ${COMPONENTS}/orb_nav/orb_nav_admission.c
    ${COMPLICATION_FONTS}
    ${COMPONENTS}/orb_ui/orb_delight_motion.c
    ${COMPONENTS}/orb_shell/orb_feedback.c
    ${COMPONENTS}/orb_shell/orb_shell_services.c
    ${COMPONENTS}/orb_shell/orb_shell_power.c
    ${COMPONENTS}/orb_power/orb_power.c
    ${COMPONENTS}/orb_power/orb_power_imu.c
    ${COMPONENTS}/orb_apps/orb_apps.c
    ${COMPONENTS}/orb_apps/orb_marble.c
    ${COMPONENTS}/orb_games/orb_game_logic.c
    ${COMPONENTS}/orb_games/orb_game_motion.c ${COMPONENTS}/orb_games/orb_game_record.c
    ${COMPONENTS}/orb_games/orb_game_interpolate.c
    ${COMPONENTS}/orb_games/orb_game_play.c
    ${COMPONENTS}/orb_time/orb_time.c
    ${COMPONENTS}/orb_time/orb_time_clock.c
    ${COMPONENTS}/orb_time/orb_time_record.c
    ${COMPONENTS}/orb_time/orb_time_voice.c
    ${COMPONENTS}/orb_time_app/orb_time_view.c
    ${COMPONENTS}/orb_time_app/orb_time_transport.c
    ${COMPONENTS}/orb_time_app/orb_time_digits.c
    ${COMPONENTS}/orb_time_app/orb_time_sheet.c
    ${COMPONENTS}/orb_time_app/orb_time_alert.c
    ${COMPONENTS}/orb_time_app/orb_time_command.c
    ${COMPONENTS}/orb_rim/orb_rim.c
    ${COMPONENTS}/orb_rim/orb_rim_math.c
    ${COMPONENTS}/orb_rim/orb_rim_feedback.c
    ${COMPONENTS}/orb_sound/orb_sound.c
    ${COMPONENTS}/orb_sound/orb_sound_preferences.c
    ${COMPONENTS}/orb_sound/orb_sound_core.c
    ${COMPONENTS}/orb_sound/orb_sound_pcm.c
    sim_apps.c
    sim_fonts.c
    ${COMPONENTS}/orb_status/src/orb_status.c
    ${COMPONENTS}/orb_shell/orb_sound_overlay.c
    ${COMPONENTS}/orb_shell/orb_system_state.c
    ${COMPONENTS}/orb_shell/orb_system_copy.c
    ${COMPONENTS}/orb_shell/orb_place_copy.c
    ${COMPONENTS}/orb_shell/orb_place_view.c
    ${COMPONENTS}/orb_app_agents/orb_usage_copy.c
    ${COMPONENTS}/orb_receipts/notification_history.c
    ${COMPONENTS}/orb_app_notifications/notifications_view.c
    ${COMPONENTS}/orb_app_notifications/notifications_screen.c
    ${COMPONENTS}/orb_receipts/receipt_copy.c
    ${COMPONENTS}/orb_app_agents/orb_agents_copy.c
    ${COMPONENTS}/orb_app_agents/orb_session_copy.c
    ${COMPONENTS}/orb_shell/orb_shell.c
    ${COMPONENTS}/orb_shell/orb_workspace.c
    ${COMPONENTS}/orb_shell/orb_workspace_snapshot.c
    ${COMPONENTS}/orb_shell/orb_neighbour.c
    ${COMPONENTS}/orb_shell/control_screen.c
    ${COMPONENTS}/orb_shell/apps_screen.c
    ${COMPONENTS}/orb_shell/stack_screen.c
    ${COMPONENTS}/orb_shell/system/mac_screen.c
    ${COMPONENTS}/orb_shell/system/boot_screen.c
    ${COMPONENTS}/orb_shell/system/system_scaffold.c
    ${COMPONENTS}/orb_shell/system/ota_screen.c
    ${COMPONENTS}/orb_shell/system/charging_screen.c
    ${COMPONENTS}/orb_shell/system/onboarding_screen.c
    ${COMPONENTS}/orb_shell/system/updates_screen.c
    ${COMPONENTS}/orb_shell/system/system_views.c
    ${COMPONENTS}/orb_shell/orb_settings_host.c
    ${COMPONENTS}/orb_shell/orb_settings_projection.c
    ${COMPONENTS}/orb_shell/orb_weather_host.c
    ${COMPONENTS}/orb_app_weather/weather_screen.c
    ${COMPONENTS}/orb_shell/orb_voice_host.c
    ${COMPONENTS}/orb_shell/orb_notifications_host.c
    ${COMPONENTS}/orb_app_voice/voice_flow.c
    ${COMPONENTS}/orb_app_voice/voice_text.c
    ${COMPONENTS}/orb_app_voice/voice_view.c
    ${COMPONENTS}/orb_app_voice/voice_screen.c
    ${COMPONENTS}/orb_app_voice/voice_manifest.c
    ${COMPONENTS}/orb_app_voice/spoken_reply_screen.c
    ${COMPONENTS}/orb_shell/orb_agents_host.c
    ${COMPONENTS}/orb_shell/orb_app_host.c
    ${COMPONENTS}/orb_shell/orb_home_host.c
    ${COMPONENTS}/orb_shell/orb_faces_host.c
    ${COMPONENTS}/orb_app_faces/face_screen.c
    ${COMPONENTS}/orb_app_faces/face_projection.c
    ${COMPONENTS}/orb_app_faces/face_manifest.c
    ${COMPONENTS}/orb_shell/orb_play_host.c
    ${COMPONENTS}/orb_app_agents/agents_common.c
    ${COMPONENTS}/orb_app_agents/agents_input.c
    ${COMPONENTS}/orb_app_agents/agents_page.c
    ${COMPONENTS}/orb_app_agents/agents_glance.c
    ${COMPONENTS}/orb_app_agents/agents_sessions.c
    ${COMPONENTS}/orb_app_agents/agents_usage.c
    ${COMPONENTS}/orb_app_agents/agents_dot.c
    ${COMPONENTS}/orb_app_agents/agents_card.c
    ${COMPONENTS}/orb_app_agents/agents_screen.c
    ${COMPONENTS}/orb_app_agents/agents_manifest.c
    ${COMPONENTS}/orb_app_play/play_screen.c
    ${COMPONENTS}/orb_app_play/play_manifest.c
    ${COMPONENTS}/orb_shell/orb_time_host.c
    ${COMPONENTS}/orb_app_time/time_screen.c
    ${COMPONENTS}/orb_app_time/time_command.c
    ${COMPONENTS}/orb_app_time/time_manifest.c
    ${COMPONENTS}/orb_app_home/home_screen.c
    ${COMPONENTS}/orb_app_home/home_context.c
    ${COMPONENTS}/orb_app_home/home_command.c
    ${COMPONENTS}/orb_app_home/home_manifest.c
    ${COMPONENTS}/orb_app_settings/about_screen.c
    ${COMPONENTS}/orb_app_settings/battery_screen.c
    ${COMPONENTS}/orb_app_settings/display_screen.c
    ${COMPONENTS}/orb_app_settings/sound_screen.c
    ${COMPONENTS}/orb_app_settings/settings_screen.c
    ${COMPONENTS}/orb_app_settings/accessibility_screen.c
    ${COMPONENTS}/orb_app_settings/agent_settings_screen.c
    ${COMPONENTS}/orb_app_settings/units_screen.c
    ${COMPONENTS}/orb_app_settings/settings_list_page.c
    ${COMPONENTS}/orb_app_settings/settings_manifest.c
    ${COMPONENTS}/orb_shell/orb_stack.c
    ${COMPONENTS}/orb_shell/orb_scroll_fixture.c
    ${COMPONENTS}/orb_model/orb_model.c
    ${COMPONENTS}/orb_model/orb_model_voice_notify.c
    ${COMPONENTS}/orb_settings/orb_settings.c
    ${COMPONENTS}/orb_faces/orb_faces_measure.c
    ${COMPONENTS}/orb_faces/orb_faces_model.c
    ${COMPONENTS}/orb_faces/orb_faces.c
    ${COMPONENTS}/orb_faces/orb_faces_motion.c
    ${COMPONENTS}/orb_faces/orb_faces_data.c
    ${COMPONENTS}/orb_home_data/orb_home.c
    ${COMPONENTS}/orb_home/orb_home_rooms.c
    ${COMPONENTS}/orb_home/orb_home_motion.c
    ${COMPONENTS}/orb_home/orb_home_view.c
    ${COMPONENTS}/orb_home/orb_home_pages.c
    ${COMPONENTS}/orb_home/orb_home_detail.c
    ${COMPONENTS}/orb_home/orb_home_detail_view.c
    ${COMPONENTS}/orb_home/orb_home_payload.c
    ${COMPONENTS}/orb_home/orb_home_command.c
    ${COMPONENTS}/orb_home/orb_home_fill.c
    ${COMPONENTS}/orb_home/orb_home_text.c
    ${COMPONENTS}/orb_ui/tokens/orb_palette.c
    ${COMPONENTS}/orb_ui/orb_ui.c
    ${COMPONENTS}/orb_ui/orb_scroll_edge.c
    ${COMPONENTS}/orb_ui/orb_round_source.c
    ${COMPONENTS}/orb_ui/orb_round_text.c
    ${COMPONENTS}/orb_ui/orb_kit_shell_controls.c
    ${COMPONENTS}/orb_ui/orb_kit_motion.c
    ${COMPONENTS}/orb_ui/orb_kit_disc_motion.c
    ${COMPONENTS}/orb_ui/orb_kit_feedback_motion.c
    ${COMPONENTS}/orb_ui/orb_kit_value_roll.c
    ${COMPONENTS}/orb_ui/orb_kit.c
    ${COMPONENTS}/orb_ui/orb_kit_rim.c
    ${COMPONENTS}/orb_ui/orb_kit_controls.c
    ${COMPONENTS}/orb_ui/orb_kit_level.c
    ${COMPONENTS}/orb_ui/orb_kit_row.c
    ${COMPONENTS}/orb_ui/orb_kit_row_effects.c
    ${COMPONENTS}/orb_ui/orb_kit_row_layout.c
    ${COMPONENTS}/orb_ui/orb_kit_text.c
    ${COMPONENTS}/orb_ui/orb_kit_value.c
    ${COMPONENTS}/orb_ui/orb_kit_scroll.c
    ${COMPONENTS}/orb_ui/orb_kit_status.c
    ${COMPONENTS}/orb_ui/orb_kit_fit.c
    ${COMPONENTS}/orb_ui/orb_kit_focus.c
    ${COMPONENTS}/orb_ui/orb_kit_pages.c
    ${COMPONENTS}/orb_ui/orb_kit_detail_page.c
    ${COMPONENTS}/orb_ui/orb_kit_state.c
    ${COMPONENTS}/orb_ui/orb_kit_receipt.c
    ${COMPONENTS}/orb_ui/orb_kit_sheet.c
    ${COMPONENTS}/orb_ui/orb_kit_confirm.c
    ${COMPONENTS}/orb_ui/orb_kit_page_label.c
    ${COMPONENTS}/orb_ui/orb_kit_page_dots.c
    ${COMPONENTS}/orb_ui/orb_input.c
    ${COMPONENTS}/orb_ui/orb_glass.c
    ${COMPONENTS}/orb_ui/orb_glass_masks.c
    ${COMPONENTS}/orb_ui/orb_glass_fused.c
    ${COMPONENTS}/orb_ui/orb_glass_sdf.c
    ${COMPONENTS}/orb_ui/orb_motion.c
    ${COMPONENTS}/orb_ui/orb_motion_math.c
    ${COMPONENTS}/orb_demo/orb_demo.c
    ${COMPONENTS}/orb_demo/orb_demo_fixture.c
    ${COMPONENTS}/orb_display/orb_display_geom.c)
target_include_directories(orb PUBLIC
    ${COMPONENTS}/orb_board/include
    ${COMPONENTS}/orb_buttons/include
    ${COMPONENTS}/orb_fonts/include
    ${COMPONENTS}/orb_fonts/src
    ${COMPONENTS}/orb_status/include
    ${COMPONENTS}/orb_apps/include
    ${COMPONENTS}/orb_games/include
    ${COMPONENTS}/orb_time/include
    ${COMPONENTS}/orb_time_app/include
    ${COMPONENTS}/orb_rim/include
    ${COMPONENTS}/orb_fs/include
    ${COMPONENTS}/orb_fs/tests/host
    ${COMPONENTS}/orb_sound/include
    ${COMPONENTS}/orb_app_settings/include
    ${COMPONENTS}/orb_app_weather/include
    ${COMPONENTS}/orb_app_home/include
    ${COMPONENTS}/orb_receipts/include
    ${COMPONENTS}/orb_app_notifications/include
    ${COMPONENTS}/orb_app_voice/include
    ${COMPONENTS}/orb_app_agents/include
    ${COMPONENTS}/orb_app_play/include
    ${COMPONENTS}/orb_app_faces/include
    ${COMPONENTS}/orb_app_time/include
    ${COMPONENTS}/orb_shell/include
    ${COMPONENTS}/orb_power/include
    ${COMPONENTS}/orb_nav/include
    ${COMPONENTS}/orb_model/include
    ${COMPONENTS}/orb_settings/include
    ${COMPONENTS}/orb_faces/include
    ${COMPONENTS}/orb_home_data/include
    ${COMPONENTS}/orb_home/include
    ${COMPONENTS}/orb_home
    ${COMPONENTS}/orb_ui/include
    ${COMPONENTS}/orb_demo/include
    ${COMPONENTS}/orb_display)
target_include_directories(orb PUBLIC ${COMPONENTS}/orb_display/include)
target_link_libraries(orb PUBLIC orb_tokens lvgl ZLIB::ZLIB pthread)
target_compile_definitions(orb PRIVATE ORB_FONT_PACK_PATH="${COMPONENTS}/orb_fonts/pack/orb_fonts.pack")
target_compile_definitions(orb PRIVATE ORB_SOUND_HOST_PACK="${CMAKE_CURRENT_SOURCE_DIR}/../tools/sounds/out/palette.orbs")
target_compile_options(orb PRIVATE ${ORB_WARNINGS})

# The registry has weak optional hooks. Include its Time provider directly so
# splitting the service object cannot silently select the empty weak defaults.
add_library(orb_time_runtime OBJECT ${COMPONENTS}/orb_time_app/orb_time.c)
target_include_directories(orb_time_runtime PRIVATE ${COMPONENTS}/orb_time/include
    ${COMPONENTS}/orb_apps/include ${COMPONENTS}/orb_fs/tests/host)
target_link_libraries(orb_time_runtime PRIVATE orb_tokens)
target_compile_options(orb_time_runtime PRIVATE ${ORB_WARNINGS})
target_sources(orb INTERFACE $<TARGET_OBJECTS:orb_time_runtime>)

add_executable(orb_sim sim_main.c sim_display.c sim_window.c sim_board.c)
target_link_libraries(orb_sim PRIVATE orb SDL2::SDL2 m)
target_compile_options(orb_sim PRIVATE ${ORB_WARNINGS})

# Tests: the fake board's physics against orb_ui's conventions, and a headless run.
enable_testing()
add_executable(test_stack tests/test_stack.c)
target_include_directories(test_stack PRIVATE ${COMPONENTS}/orb_shell)
target_link_libraries(test_stack PRIVATE orb m)
target_compile_options(test_stack PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME stack_relevance COMMAND test_stack)
add_executable(test_stack_reader tests/test_stack_reader.c sim_display.c sim_board.c)
target_include_directories(test_stack_reader PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_stack_reader PRIVATE orb m)
target_compile_options(test_stack_reader PRIVATE ${ORB_WARNINGS} -UNDEBUG)
foreach(contact scroll top bottom rim preview static)
    add_test(NAME stack_reader_${contact} COMMAND test_stack_reader ${contact})
endforeach()
add_executable(test_senses ${COMPONENTS}/orb_board/orb_senses.c
    ${COMPONENTS}/orb_board/tests/host/test_senses.c)
target_include_directories(test_senses PRIVATE ${COMPONENTS}/orb_board/include)
target_link_libraries(test_senses PRIVATE m)
target_compile_options(test_senses PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME senses COMMAND test_senses)
add_executable(test_idle_policy ${FIRMWARE}/main/tests/host/test_idle_policy.c
    ${COMPONENTS}/orb_buttons/orb_buttons.c)
target_include_directories(test_idle_policy PRIVATE ${FIRMWARE}/main
    ${COMPONENTS}/orb_power/include ${COMPONENTS}/orb_buttons/include)
target_compile_options(test_idle_policy PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME idle_policy COMMAND test_idle_policy)
add_test(NAME input_modes COMMAND python3 ${FIRMWARE}/main/tests/host/test_input_modes.py)
add_executable(test_power_adapter ${COMPONENTS}/orb_board/tests/host/test_power_adapter.c
    ${COMPONENTS}/orb_power/orb_power_board.c ${COMPONENTS}/orb_power/orb_power.c
    ${COMPONENTS}/orb_power/orb_power_imu.c)
target_include_directories(test_power_adapter PRIVATE ${COMPONENTS}/orb_power/include
    ${COMPONENTS}/orb_power/tests/host/stubs)
target_link_libraries(test_power_adapter PRIVATE m)
target_compile_options(test_power_adapter PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME power_adapter COMMAND test_power_adapter)
add_test(NAME board_panel_power COMMAND python3 ${COMPONENTS}/orb_board/tests/host/test_panel_power.py)
add_test(NAME board_qmi_control COMMAND python3 ${COMPONENTS}/orb_board/tests/host/test_qmi_control.py)
add_test(NAME board_button_edges COMMAND python3 ${COMPONENTS}/orb_board/tests/host/test_button_edges.py)
add_test(NAME board_sound_idle COMMAND python3 ${COMPONENTS}/orb_board/tests/host/test_sound_idle.py)
add_test(NAME board_audio COMMAND python3 ${COMPONENTS}/orb_board/tests/host/test_audio.py)
add_executable(test_dark_refresh ${COMPONENTS}/orb_display/tests/host/test_dark_refresh.c)
target_include_directories(test_dark_refresh PRIVATE ${COMPONENTS}/orb_display/include)
target_link_libraries(test_dark_refresh PRIVATE lvgl m)
target_compile_options(test_dark_refresh PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME dark_refresh COMMAND test_dark_refresh)
add_executable(test_game_idle ${COMPONENTS}/orb_board/tests/host/test_game_idle.c
    sim_display.c sim_board.c ${COMPONENTS}/orb_games/orb_games.c)
target_include_directories(test_game_idle PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_game_idle PRIVATE orb_games_view)
target_compile_options(test_game_idle PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME game_idle COMMAND test_game_idle)
add_executable(test_battery ${COMPONENTS}/orb_board/orb_battery.c
    ${COMPONENTS}/orb_board/tests/host/test_battery.c)
target_include_directories(test_battery PRIVATE ${COMPONENTS}/orb_board/include)
target_link_libraries(test_battery PRIVATE m)
target_compile_options(test_battery PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME battery COMMAND test_battery)
add_executable(test_audio_levels ${COMPONENTS}/orb_board/tests/host/test_audio_levels.c)
target_include_directories(test_audio_levels PRIVATE ${COMPONENTS}/orb_board)
target_compile_options(test_audio_levels PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME audio_levels COMMAND test_audio_levels)
add_test(NAME mic_gain COMMAND python3
    ${COMPONENTS}/orb_board/tests/host/test_mic_gain.py)
add_executable(test_speech_core ${COMPONENTS}/orb_voice/speech_core.c
    ${COMPONENTS}/orb_voice/mic_dsp.c ${COMPONENTS}/orb_voice/tests/host/test_speech.c)
target_include_directories(test_speech_core PRIVATE ${COMPONENTS}/orb_voice ${COMPONENTS}/orb_board/include)
target_link_libraries(test_speech_core PRIVATE m)
target_compile_options(test_speech_core PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME speech_core COMMAND test_speech_core)
add_executable(test_voice_core ${COMPONENTS}/orb_voice/voice_core.c
    ${COMPONENTS}/orb_voice/tests/host/test_voice.c)
target_include_directories(test_voice_core PRIVATE ${COMPONENTS}/orb_voice ${COMPONENTS}/orb_model/include)
target_compile_options(test_voice_core PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME voice_core COMMAND test_voice_core)
add_test(NAME voice_event_callbacks COMMAND python3 ${COMPONENTS}/orb_voice/tests/host/test_events.py)
add_executable(test_sound_palette ${COMPONENTS}/orb_sound/orb_sound.c
    ${COMPONENTS}/orb_sound/orb_sound_preferences.c
    ${COMPONENTS}/orb_sound/orb_sound_core.c ${COMPONENTS}/orb_sound/orb_sound_pcm.c
    ${COMPONENTS}/orb_sound/tests/host/test_sound.c)
target_include_directories(test_sound_palette PRIVATE ${COMPONENTS}/orb_sound ${COMPONENTS}/orb_sound/include)
target_compile_definitions(test_sound_palette PRIVATE ORB_SOUND_HOST_PACK="${CMAKE_CURRENT_SOURCE_DIR}/../tools/sounds/out/palette.orbs")
target_compile_options(test_sound_palette PRIVATE ${ORB_WARNINGS} -UNDEBUG)
target_link_libraries(test_sound_palette PRIVATE pthread m)
add_test(NAME sound_palette COMMAND test_sound_palette)
add_test(NAME sound_preferences_storage COMMAND python3
    ${COMPONENTS}/orb_sound/tests/host/test_preferences_storage.py)
set_tests_properties(sound_palette PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)
add_executable(test_memory_budget ${COMPONENTS}/orb_memory/tests/host/test_memory_budget.c)
target_link_libraries(test_memory_budget PRIVATE orb_memory pthread)
target_compile_options(test_memory_budget PRIVATE ${ORB_WARNINGS})
add_test(NAME memory_budget COMMAND test_memory_budget)
add_executable(test_memory_esp ${COMPONENTS}/orb_memory/tests/host/test_memory_esp.c
    ${COMPONENTS}/orb_memory/orb_memory.c)
target_include_directories(test_memory_esp PRIVATE ${COMPONENTS}/orb_memory/include
    ${COMPONENTS}/orb_memory/tests/host/stubs)
target_compile_definitions(test_memory_esp PRIVATE ESP_PLATFORM=1)
target_compile_options(test_memory_esp PRIVATE ${ORB_WARNINGS})
add_test(NAME memory_esp COMMAND test_memory_esp)
add_executable(test_memory_phases ${COMPONENTS}/orb_mem/tests/host/test_mem.c)
target_include_directories(test_memory_phases PRIVATE ${COMPONENTS}/orb_mem/include
    ${COMPONENTS}/orb_mem/tests/host/stubs)
target_compile_definitions(test_memory_phases PRIVATE CONFIG_ORB_DEVTOOLS=1)
target_compile_options(test_memory_phases PRIVATE ${ORB_WARNINGS})
add_test(NAME memory_phases COMMAND test_memory_phases)
add_executable(test_snapshot_bands tests/test_snapshot_bands.c)
target_include_directories(test_snapshot_bands PRIVATE ${COMPONENTS}/orb_display)
target_link_libraries(test_snapshot_bands PRIVATE lvgl m)
target_compile_options(test_snapshot_bands PRIVATE ${ORB_WARNINGS})
add_test(NAME snapshot_bands COMMAND test_snapshot_bands)
add_executable(test_refresh_phases tests/test_refresh_phases.c)
target_link_libraries(test_refresh_phases PRIVATE lvgl m)
target_compile_options(test_refresh_phases PRIVATE ${ORB_WARNINGS})
add_test(NAME refresh_phases COMMAND test_refresh_phases)
foreach(mode default ax3 opaque ax3-opaque)
    set(SYNTHETIC_ARGS)
    if(mode MATCHES "ax3")
        list(APPEND SYNTHETIC_ARGS ax3)
    endif()
    if(mode MATCHES "opaque")
        list(APPEND SYNTHETIC_ARGS rt)
    endif()
    add_test(NAME synthetic_input_${mode} COMMAND test_shell --synthetic
        ${CMAKE_CURRENT_BINARY_DIR}/synthetic-card-${mode}.png ${SYNTHETIC_ARGS})
endforeach()
add_executable(test_board tests/test_board.c sim_board.c ${COMPONENTS}/orb_ui/orb_motion_math.c)
target_include_directories(test_board PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${COMPONENTS}/orb_ui ${COMPONENTS}/orb_ui/tests/host)
target_link_libraries(test_board PRIVATE m)
target_compile_options(test_board PRIVATE ${ORB_WARNINGS})
add_test(NAME board COMMAND test_board)
add_executable(test_ambient_redraw tests/test_ambient_redraw.c sim_display.c sim_board.c)
target_include_directories(test_ambient_redraw PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_ambient_redraw PRIVATE orb m)
target_compile_options(test_ambient_redraw PRIVATE ${ORB_WARNINGS})
add_test(NAME ambient_redraw COMMAND test_ambient_redraw)
add_test(NAME headless COMMAND orb_sim --headless --tilt 500=20,10 --shot 1000=${CMAKE_CURRENT_BINARY_DIR}/test.png)

add_executable(test_scene tests/test_scene.c sim_display.c sim_board.c)
target_include_directories(test_scene PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_scene PRIVATE orb m)
target_compile_options(test_scene PRIVATE ${ORB_WARNINGS})
add_test(NAME scene COMMAND test_scene)
add_test(NAME reduced_motion COMMAND test_scene --reduce-motion)
add_test(NAME raster_scale COMMAND test_scene --scale)

if(ORB_RENDER_ROWS_AT_SIZE)
    add_executable(test_rows_at_size tests/test_rows_at_size.c sim_display.c sim_board.c)
    target_include_directories(test_rows_at_size PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test_rows_at_size PRIVATE orb m)
    target_compile_options(test_rows_at_size PRIVATE ${ORB_WARNINGS})
    add_test(NAME rows_at_size COMMAND test_rows_at_size)
endif()

if(ORB_RENDER_FUSED_GLASS)
    add_executable(test_fused_glass tests/test_fused_glass.c sim_display.c sim_board.c)
    target_include_directories(test_fused_glass PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test_fused_glass PRIVATE orb m)
    target_compile_options(test_fused_glass PRIVATE ${ORB_WARNINGS})
    add_test(NAME fused_glass COMMAND test_fused_glass)
endif()

add_executable(test_pixels tests/test_pixels.c)
target_link_libraries(test_pixels PRIVATE lvgl m)
target_compile_options(test_pixels PRIVATE ${ORB_WARNINGS})
add_test(NAME chrome_initial COMMAND orb_sim --headless --shot 0=${CMAKE_CURRENT_BINARY_DIR}/chrome-initial.png)
add_test(NAME chrome_scrolled COMMAND orb_sim --headless --shot 8100=${CMAKE_CURRENT_BINARY_DIR}/chrome-scrolled.png)
set_tests_properties(chrome_initial chrome_scrolled PROPERTIES FIXTURES_SETUP chrome_frames)
add_test(NAME chrome_pixels COMMAND test_pixels ${CMAKE_CURRENT_BINARY_DIR}/chrome-initial.png ${CMAKE_CURRENT_BINARY_DIR}/chrome-scrolled.png)
set_tests_properties(chrome_pixels PROPERTIES FIXTURES_REQUIRED chrome_frames)

add_executable(test_blur ${COMPONENTS}/orb_ui/tests/host/test_blur.c)
target_link_libraries(test_blur PRIVATE lvgl m)
target_compile_options(test_blur PRIVATE ${ORB_WARNINGS})
add_test(NAME blur COMMAND test_blur)

add_executable(test_solid_blur tests/test_solid_blur.c)
target_link_libraries(test_solid_blur PRIVATE lvgl m)
target_compile_options(test_solid_blur PRIVATE ${ORB_WARNINGS})
add_test(NAME solid_blur COMMAND test_solid_blur)

add_executable(test_pacing tests/test_pacing.c)
target_include_directories(test_pacing PRIVATE ${COMPONENTS}/orb_display)
target_link_libraries(test_pacing PRIVATE lvgl m)
target_compile_options(test_pacing PRIVATE ${ORB_WARNINGS})
add_test(NAME pacing COMMAND test_pacing)

# System screens use the same fonts, status widgets, shell and renderer as firmware.
add_executable(orb_shell_sim shell_main.c sim_display.c sim_board.c)
target_link_libraries(orb_shell_sim PRIVATE orb m)
target_compile_options(orb_shell_sim PRIVATE ${ORB_WARNINGS})
foreach(scene home setup setup-retry pairing connecting offline identify service-error update downloading verifying ready update-error
    face demo apps apps-next agents agents-next session session-summary stack approval approval-actions choice choice-options choice-options-next
    message attention notifications control-center control-silent control-motion usage charging
    agents-empty sessions-empty dot-empty stack-empty notifications-empty
    stack-day stack-date stack-timer stack-stopwatch stack-usage
    settings settings-next display accessibility access-bold access-motion access-transparency access-contrast about about-licenses dot voice play updates
    voice-starting voice-recording voice-transcribing voice-confirming voice-sending voice-error
    voice-no-speech voice-network voice-unsupported
    agents-idle agents-idle-row idle-loading idle-page idle-more idle-end idle-empty idle-error idle-more-pending idle-more-error idle-detail idle-reply session-outside-focus
    sessions-settings sessions-codex sessions-dot sessions-recent sessions-pending sessions-error sessions-off sessions-touched sessions-recent-off sessions-recent-five sessions-recent-thirty
    answer-queued answer-delivered answer-undelivered answer-other answer-rejected card-cancelled
    message-delivered message-timeout)
    foreach(size default ax3)
        add_test(NAME shell_${scene}_${size} COMMAND orb_shell_sim ${scene}
            ${CMAKE_CURRENT_BINARY_DIR}/shell-${scene}-${size}.png ${size})
    endforeach()
    add_test(NAME shell_${scene}_ax3_bold_rt COMMAND orb_shell_sim ${scene}
        ${CMAKE_CURRENT_BINARY_DIR}/shell-${scene}-ax3-bold-rt.png ax3 bold rt)
endforeach()

foreach(scene face agents approval-actions)
    add_test(NAME shell_${scene}_opaque COMMAND orb_shell_sim ${scene}
        ${CMAKE_CURRENT_BINARY_DIR}/shell-${scene}-opaque.png ax3 bold rt rm)
endforeach()

# Touch input and live-state recovery, including irreversible-action confirmation.
add_executable(test_shell tests/test_shell.c sim_display.c sim_board.c)
target_include_directories(test_shell PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_shell PRIVATE orb m)
target_compile_options(test_shell PRIVATE ${ORB_WARNINGS})
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/offline-default
    ${CMAKE_CURRENT_BINARY_DIR}/offline-ax3-bold-rt
    ${CMAKE_CURRENT_BINARY_DIR}/offline-reduce-motion)
add_test(NAME offline_contract_default COMMAND test_shell --offline
    ${CMAKE_CURRENT_BINARY_DIR}/offline-default)
add_test(NAME offline_contract_ax3_bold_rt COMMAND test_shell --offline
    ${CMAKE_CURRENT_BINARY_DIR}/offline-ax3-bold-rt ax3 bold rt)
add_test(NAME offline_contract_reduce_motion COMMAND test_shell --offline
    ${CMAKE_CURRENT_BINARY_DIR}/offline-reduce-motion ax3 bold rt rm)
add_test(NAME shell_contract COMMAND test_shell)
set_tests_properties(shell_contract PROPERTIES TIMEOUT 60)
add_test(NAME shell_preferences COMMAND test_shell --preferences)
add_test(NAME shell_flow COMMAND orb_shell_sim --flow ${CMAKE_CURRENT_BINARY_DIR})

# Same-source before/after round-first screenshots, isolated synthetic data.
add_executable(test_round_shell tests/test_round_shell.c sim_display.c sim_board.c)
target_include_directories(test_round_shell PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_round_shell PRIVATE orb m)
target_compile_options(test_round_shell PRIVATE ${ORB_WARNINGS})
foreach(mode default ax3 ax3-bold-rt)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/round-shell/after/${mode})
    add_test(NAME round_shell_${mode} COMMAND test_round_shell
        ${CMAKE_CURRENT_BINARY_DIR}/round-shell/after/${mode} ${mode})
endforeach()

# App-open feedback must precede cold construction and survive cancellation.
add_executable(test_open_latency tests/test_open_latency.c sim_display.c sim_board.c)
target_include_directories(test_open_latency PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_open_latency PRIVATE orb m)
target_compile_options(test_open_latency PRIVATE ${ORB_WARNINGS})
if(ORB_OPEN_LATENCY_FIXES)
    add_test(NAME open_latency COMMAND test_open_latency ${CMAKE_CURRENT_BINARY_DIR})
endif()

# Compile the actual console benchmark, with virtual RTOS waits and panel
# completion. Source-only navigation tests miss timers that beat RENDER_START.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${COMPONENTS}/orb_devtools/orb_devtools.c)
file(READ ${COMPONENTS}/orb_devtools/orb_devtools.c DEVTOOLS_SOURCE)
string(FIND "${DEVTOOLS_SOURCE}" "/* Opening uses the live shell" OPEN_BEGIN)
string(FIND "${DEVTOOLS_SOURCE}" "/* End live-shell opening benchmark. */" OPEN_END)
if(OPEN_BEGIN LESS 0 OR OPEN_END LESS OPEN_BEGIN)
    message(FATAL_ERROR "Opening benchmark source boundaries are missing or out of order")
endif()
math(EXPR OPEN_LENGTH "${OPEN_END} - ${OPEN_BEGIN}")
string(SUBSTRING "${DEVTOOLS_SOURCE}" ${OPEN_BEGIN} ${OPEN_LENGTH} OPEN_SOURCE)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/devtools_open_benchmark.inc "${OPEN_SOURCE}")
add_executable(test_open_benchmark tests/test_open_benchmark.c sim_display.c sim_board.c)
target_include_directories(test_open_benchmark PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR} ${COMPONENTS}/orb_devtools
    ${COMPONENTS}/orb_display/include ${COMPONENTS}/orb_mem/tests/host/stubs)
target_link_libraries(test_open_benchmark PRIVATE orb m)
target_compile_options(test_open_benchmark PRIVATE ${ORB_WARNINGS})
if(ORB_OPEN_LATENCY_FIXES)
    add_test(NAME open_benchmark COMMAND test_open_benchmark)
endif()

# Execute the actual worker rather than a second copy of its step-budget loop.
string(FIND "${DEVTOOLS_SOURCE}" "static void game_benchmark_drive(" GAME_WORKER_BEGIN)
string(FIND "${DEVTOOLS_SOURCE}" "static void game_benchmark_render_latest(" GAME_WORKER_END)
if(GAME_WORKER_BEGIN LESS 0 OR GAME_WORKER_END LESS GAME_WORKER_BEGIN)
    message(FATAL_ERROR "Game benchmark worker source boundaries are missing or out of order")
endif()
math(EXPR GAME_WORKER_LENGTH "${GAME_WORKER_END} - ${GAME_WORKER_BEGIN}")
string(SUBSTRING "${DEVTOOLS_SOURCE}" ${GAME_WORKER_BEGIN} ${GAME_WORKER_LENGTH} GAME_WORKER_SOURCE)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/devtools_game_worker.inc "${GAME_WORKER_SOURCE}")
add_executable(test_game_benchmark_worker tests/test_game_benchmark_worker.c
    ${COMPONENTS}/orb_games/orb_game_logic.c
    ${COMPONENTS}/orb_games/orb_game_motion.c ${COMPONENTS}/orb_games/orb_game_record.c ${COMPONENTS}/orb_games/orb_game_interpolate.c)
target_include_directories(test_game_benchmark_worker PRIVATE ${CMAKE_CURRENT_BINARY_DIR}
    ${COMPONENTS}/orb_games/include)
target_compile_options(test_game_benchmark_worker PRIVATE ${ORB_WARNINGS})
target_link_libraries(test_game_benchmark_worker PRIVATE m)
add_test(NAME game_benchmark_worker COMMAND test_game_benchmark_worker)

string(FIND "${DEVTOOLS_SOURCE}" "static void game_benchmark_refresh_start(lv_event_t *event)\n{" GAME_CALLBACK_BEGIN)
string(FIND "${DEVTOOLS_SOURCE}" "static void game_benchmark_tick(" GAME_CALLBACK_END)
if(GAME_CALLBACK_BEGIN LESS 0 OR GAME_CALLBACK_END LESS GAME_CALLBACK_BEGIN)
    message(FATAL_ERROR "Game benchmark callback source boundaries are missing or out of order")
endif()
math(EXPR GAME_CALLBACK_LENGTH "${GAME_CALLBACK_END} - ${GAME_CALLBACK_BEGIN}")
string(SUBSTRING "${DEVTOOLS_SOURCE}" ${GAME_CALLBACK_BEGIN} ${GAME_CALLBACK_LENGTH} GAME_CALLBACK_SOURCE)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/devtools_game_callbacks.inc "${GAME_CALLBACK_SOURCE}")
string(FIND "${DEVTOOLS_SOURCE}" "static bool game_benchmark_present_due(" GAME_DUE_BEGIN)
string(FIND "${DEVTOOLS_SOURCE}" "static void game_benchmark_render_latest(" GAME_DUE_END)
if(GAME_DUE_BEGIN LESS 0 OR GAME_DUE_END LESS GAME_DUE_BEGIN)
    message(FATAL_ERROR "Game benchmark producer source boundaries are missing or out of order")
endif()
math(EXPR GAME_DUE_LENGTH "${GAME_DUE_END} - ${GAME_DUE_BEGIN}")
string(SUBSTRING "${DEVTOOLS_SOURCE}" ${GAME_DUE_BEGIN} ${GAME_DUE_LENGTH} GAME_DUE_SOURCE)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/devtools_game_due.inc "${GAME_DUE_SOURCE}")
add_executable(test_game_benchmark_callbacks tests/test_game_benchmark_callbacks.c)
target_include_directories(test_game_benchmark_callbacks PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_compile_options(test_game_benchmark_callbacks PRIVATE ${ORB_WARNINGS})
add_test(NAME game_benchmark_callbacks COMMAND test_game_benchmark_callbacks)
add_test(NAME devtools_wake COMMAND python3 ${COMPONENTS}/orb_devtools/tests/host/test_wake.py)
add_test(NAME devtools_sender COMMAND python3 ${COMPONENTS}/orb_devtools/tests/host/test_sender.py)
add_test(NAME devtools_diagnostics COMMAND python3 ${COMPONENTS}/orb_devtools/tests/host/test_diagnostics.py)
add_test(NAME devtools_heap COMMAND python3 ${COMPONENTS}/orb_devtools/tests/host/test_heap.py)
add_test(NAME game_beads_diagnostics COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_game_beads_diagnostics.py)

# A temporary benchmark scene must not keep timers/animations after deletion.
add_executable(test_demo_lifecycle tests/test_demo_lifecycle.c sim_display.c sim_board.c)
target_include_directories(test_demo_lifecycle PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_demo_lifecycle PRIVATE orb m)
target_compile_options(test_demo_lifecycle PRIVATE ${ORB_WARNINGS})
add_test(NAME demo_lifecycle COMMAND test_demo_lifecycle)

# Splitting the existing draw arena must preserve whole-glass refresh pixels.
add_executable(test_display_buffers tests/test_display_buffers.c)
target_link_libraries(test_display_buffers PRIVATE orb m)
target_compile_options(test_display_buffers PRIVATE ${ORB_WARNINGS})
add_test(NAME display_buffers COMMAND test_display_buffers ${CMAKE_CURRENT_BINARY_DIR}/adaptive)

add_executable(test_snapshot_overlap tests/test_snapshot_overlap.c)
target_link_libraries(test_snapshot_overlap PRIVATE orb m)
target_compile_options(test_snapshot_overlap PRIVATE ${ORB_WARNINGS})
add_test(NAME snapshot_overlap COMMAND test_snapshot_overlap)

# A queued dynamic label must not borrow a buffer temporarily changed by size queries.
add_executable(test_label_text_lifetime tests/test_label_text_lifetime.c)
target_link_libraries(test_label_text_lifetime PRIVATE lvgl m)
target_compile_options(test_label_text_lifetime PRIVATE ${ORB_WARNINGS})
add_test(NAME label_text_lifetime COMMAND test_label_text_lifetime)
set_tests_properties(label_text_lifetime PROPERTIES TIMEOUT 5)

# Bit-exact blur corpus and a non-gating timing tool.
add_executable(test_blur_exact ${COMPONENTS}/orb_ui/tests/host/test_blur_exact.c)
target_link_libraries(test_blur_exact PRIVATE lvgl m)
target_compile_options(test_blur_exact PRIVATE ${ORB_WARNINGS})
add_test(NAME blur_exact COMMAND test_blur_exact)

add_executable(test_blur_cache ${COMPONENTS}/orb_ui/tests/host/test_blur_cache.c)
target_link_libraries(test_blur_cache PRIVATE lvgl m pthread)
target_compile_options(test_blur_cache PRIVATE ${ORB_WARNINGS})
add_test(NAME blur_cache COMMAND test_blur_cache)
add_executable(bench_blur ${COMPONENTS}/orb_ui/tests/host/bench_blur.c)
target_link_libraries(bench_blur PRIVATE lvgl m)
target_compile_options(bench_blur PRIVATE ${ORB_WARNINGS})

# Wave 2 visual regressions and non-gating virtual-scene timing.
add_executable(test_scroll_redraw tests/test_scroll_redraw.c sim_display.c sim_board.c)
target_include_directories(test_scroll_redraw PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_scroll_redraw PRIVATE orb m)
target_compile_options(test_scroll_redraw PRIVATE ${ORB_WARNINGS})
add_test(NAME scroll_redraw COMMAND test_scroll_redraw)
add_test(NAME draw_profile_events COMMAND python3
    ${COMPONENTS}/orb_display/tests/host/test_draw_profile.py)
add_test(NAME panel_dma_timing COMMAND python3
    ${COMPONENTS}/orb_board/tests/host/test_panel_timing.py)

add_executable(test_scroll_damage tests/test_scroll_damage.c)
target_link_libraries(test_scroll_damage PRIVATE lvgl m)
target_compile_options(test_scroll_damage PRIVATE ${ORB_WARNINGS})
add_test(NAME scroll_damage COMMAND test_scroll_damage)
add_executable(test_damage_overflow tests/test_damage_overflow.c)
target_link_libraries(test_damage_overflow PRIVATE lvgl m)
target_compile_options(test_damage_overflow PRIVATE ${ORB_WARNINGS})
add_test(NAME damage_overflow COMMAND test_damage_overflow)

add_executable(bench_glass tests/bench_glass.c sim_display.c sim_board.c)
target_include_directories(bench_glass PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bench_glass PRIVATE orb m)
target_compile_options(bench_glass PRIVATE ${ORB_WARNINGS})

add_executable(glass_crops tests/glass_crops.c)
target_link_libraries(glass_crops PRIVATE lvgl m)
target_compile_options(glass_crops PRIVATE ${ORB_WARNINGS})

add_executable(test_glass_shadow tests/test_glass_shadow.c)
target_link_libraries(test_glass_shadow PRIVATE lvgl m)
target_compile_options(test_glass_shadow PRIVATE ${ORB_WARNINGS})
add_test(NAME glass_shadow COMMAND test_glass_shadow)

add_executable(test_round_helpers tests/test_round_helpers.c sim_display.c sim_board.c)
target_include_directories(test_round_helpers PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_round_helpers PRIVATE orb m)
target_compile_options(test_round_helpers PRIVATE ${ORB_WARNINGS})
add_test(NAME round_helpers COMMAND test_round_helpers)
add_test(NAME glass_timing COMMAND bench_glass)

add_executable(test_raster_scale_cache tests/test_raster_scale_cache.c sim_display.c sim_board.c)
target_include_directories(test_raster_scale_cache PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_raster_scale_cache PRIVATE orb m)
target_compile_options(test_raster_scale_cache PRIVATE ${ORB_WARNINGS})
add_test(NAME raster_scale_cache COMMAND test_raster_scale_cache)
add_test(NAME raster_scale_cache_native COMMAND test_raster_scale_cache --native)

add_executable(test_rgb565_scale_fast tests/test_rgb565_scale_fast.c)
target_link_libraries(test_rgb565_scale_fast PRIVATE lvgl m)
target_compile_options(test_rgb565_scale_fast PRIVATE ${ORB_WARNINGS})
add_test(NAME rgb565_scale_fast COMMAND test_rgb565_scale_fast)

add_executable(test_speed_motion tests/test_speed_motion.c)
target_link_libraries(test_speed_motion PRIVATE lvgl m)
target_compile_options(test_speed_motion PRIVATE ${ORB_WARNINGS})
add_test(NAME speed_motion COMMAND test_speed_motion)

add_executable(test_glass_design tests/test_glass_design.c sim_display.c sim_board.c)
target_include_directories(test_glass_design PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_glass_design PRIVATE orb m)
target_compile_options(test_glass_design PRIVATE ${ORB_WARNINGS})
foreach(surface face list card)
    foreach(mode default ax3 opaque ax3-opaque)
        add_test(NAME glass_design_${surface}_${mode} COMMAND test_glass_design ${surface}
            ${CMAKE_CURRENT_BINARY_DIR}/gate-${surface}-${mode}.png ${mode})
    endforeach()
    add_test(NAME glass_design_${surface}_bold COMMAND test_glass_design ${surface}
        ${CMAKE_CURRENT_BINARY_DIR}/gate-${surface}-bold.png default bold)
    add_test(NAME glass_design_${surface}_ax3_opaque_bold COMMAND test_glass_design ${surface}
        ${CMAKE_CURRENT_BINARY_DIR}/gate-${surface}-ax3-opaque-bold.png ax3-opaque bold)
endforeach()

# Public materialisation/lifecycle contract, including accessibility retargets.
add_executable(test_glass_materialise tests/test_glass_materialise.c sim_display.c sim_board.c)
target_include_directories(test_glass_materialise PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_glass_materialise PRIVATE orb m)
target_compile_options(test_glass_materialise PRIVATE ${ORB_WARNINGS})
add_test(NAME glass_materialise COMMAND test_glass_materialise)

# Architecture accounting fixtures keep the original scroll/glass demo intact.
add_executable(test_benchmark_fixtures tests/test_benchmark_fixtures.c sim_display.c sim_board.c)
target_include_directories(test_benchmark_fixtures PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_benchmark_fixtures PRIVATE orb m)
target_compile_options(test_benchmark_fixtures PRIVATE ${ORB_WARNINGS})
add_test(NAME benchmark_fixtures COMMAND test_benchmark_fixtures ${CMAKE_CURRENT_BINARY_DIR})

if(ORB_DISPLAY_RETAINED_MATERIAL)
    add_executable(test_retained_material tests/test_retained_material.c sim_display.c sim_board.c)
    target_include_directories(test_retained_material PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test_retained_material PRIVATE orb m)
    target_compile_options(test_retained_material PRIVATE ${ORB_WARNINGS})
    add_test(NAME retained_material COMMAND test_retained_material)
    add_test(NAME retained_material_native COMMAND test_retained_material --native)
endif()

# Firmware-only font transactions use a fake main owner with real LVGL and glass leases.
add_executable(test_async_fonts ${COMPONENTS}/orb_fonts/tests/host/test_async_settings.c sim_display.c sim_board.c)
target_include_directories(test_async_fonts PRIVATE ${COMPONENTS}/orb_fonts/tests/host/async_stubs ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(test_async_fonts PRIVATE ORB_ASYNC_TEST_PACK_PATH="${COMPONENTS}/orb_fonts/pack/orb_fonts.pack")
target_link_libraries(test_async_fonts PRIVATE orb m)
target_compile_options(test_async_fonts PRIVATE ${ORB_WARNINGS})
add_test(NAME async_fonts COMMAND test_async_fonts)
add_test(NAME async_fonts_partial COMMAND test_async_fonts --partial-boot)
add_test(NAME async_fonts_control COMMAND test_async_fonts --lazy-control)
add_test(NAME async_fonts_fallback COMMAND test_async_fonts --fallback-closure)
add_test(NAME fonts_pack_memory COMMAND make -C ${COMPONENTS}/orb_fonts/tests/host
    BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}/fonts-host CC=${CMAKE_C_COMPILER} -j4 test)
add_executable(test_mapped_pixels ${COMPONENTS}/orb_fonts/tests/host/test_mapped_pixels.c
    ${COMPONENTS}/orb_fonts/src/orb_fonts_pack.c)
target_include_directories(test_mapped_pixels PRIVATE ${COMPONENTS}/orb_fonts/include ${COMPONENTS}/orb_fonts/src)
target_compile_definitions(test_mapped_pixels PRIVATE ORB_FONTS_TEST_MAPPING)
target_link_libraries(test_mapped_pixels PRIVATE lvgl m)
target_compile_options(test_mapped_pixels PRIVATE ${ORB_WARNINGS})
add_test(NAME fonts_mapped_pixels COMMAND test_mapped_pixels ${COMPONENTS}/orb_fonts/pack/orb_fonts.pack)

add_executable(test_delight_motion ${COMPONENTS}/orb_ui/orb_delight_motion.c
    ${COMPONENTS}/orb_shell/tests/test_delight_motion.c)
target_include_directories(test_delight_motion PRIVATE ${COMPONENTS}/orb_ui/include)
target_link_libraries(test_delight_motion PRIVATE m)
target_link_libraries(test_delight_motion PRIVATE orb_tokens)
target_compile_options(test_delight_motion PRIVATE ${ORB_WARNINGS})
add_test(NAME delight_motion COMMAND test_delight_motion)
add_test(NAME shell_delight COMMAND orb_shell_sim --delight ${CMAKE_CURRENT_BINARY_DIR})
add_executable(test_delight tests/test_delight.c sim_display.c sim_board.c)
target_include_directories(test_delight PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_delight PRIVATE orb m)
target_compile_options(test_delight PRIVATE ${ORB_WARNINGS})
add_test(NAME delight_contract COMMAND test_delight)
foreach(size default ax3)
    add_test(NAME shell_play-registered_${size} COMMAND orb_shell_sim play-registered
        ${CMAKE_CURRENT_BINARY_DIR}/shell-play-registered-${size}.png ${size})
endforeach()
foreach(scene face agents approval)
    add_test(NAME shell_${scene}_ic COMMAND orb_shell_sim ${scene}
        ${CMAKE_CURRENT_BINARY_DIR}/shell-${scene}-ic.png ic)
    add_test(NAME shell_${scene}_ax3_bold_rt_ic COMMAND orb_shell_sim ${scene}
        ${CMAKE_CURRENT_BINARY_DIR}/shell-${scene}-ax3-bold-rt-ic.png ax3 bold rt ic)
endforeach()
add_test(NAME shell_play-registered_ax3_bold_rt COMMAND orb_shell_sim play-registered
    ${CMAKE_CURRENT_BINARY_DIR}/shell-play-registered-ax3-bold-rt.png ax3 bold rt)
# Offline games share the exact firmware logic and partial-damage sprite view.
add_library(orb_games_view STATIC
    ${COMPONENTS}/orb_app_play/game_screen.c
    ${COMPONENTS}/orb_games/orb_game_view.c
    ${COMPONENTS}/orb_games/orb_game_sprite_draw.c
    ${COMPONENTS}/orb_games/orb_game_chrome.c
    ${COMPONENTS}/orb_games/orb_game_native.c
    ${COMPONENTS}/orb_games/orb_game_pop.c
    ${COMPONENTS}/orb_games/orb_game_beads_draw.c
    ${COMPONENTS}/orb_games/orb_game_orbit.c
    ${COMPONENTS}/orb_games/orb_game_ring.c)
target_include_directories(orb_games_view PUBLIC ${COMPONENTS}/orb_games/include)
target_link_libraries(orb_games_view PUBLIC orb m)
target_compile_options(orb_games_view PRIVATE ${ORB_WARNINGS})
# F16: link the production registry provider/controller, rather than the legacy
# shell-only Marble proof. The executable provides scoped in-memory storage.
add_executable(orb_game_flow_sim game_flow_main.c sim_display.c sim_board.c
    ${COMPONENTS}/orb_games/orb_games.c)
target_link_libraries(orb_game_flow_sim PRIVATE orb_games_view)
target_compile_options(orb_game_flow_sim PRIVATE ${ORB_WARNINGS})
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/game-flow-default
    ${CMAKE_CURRENT_BINARY_DIR}/game-flow-ax3-bold-rt)
add_test(NAME game_flow_default COMMAND orb_game_flow_sim
    ${CMAKE_CURRENT_BINARY_DIR}/game-flow-default)
add_test(NAME game_flow_ax3_bold_rt COMMAND orb_game_flow_sim
    ${CMAKE_CURRENT_BINARY_DIR}/game-flow-ax3-bold-rt ax3 bold rt)
add_executable(orb_games_sim games_main.c sim_display.c sim_window.c sim_board.c)
target_link_libraries(orb_games_sim PRIVATE orb_games_view SDL2::SDL2)
target_compile_options(orb_games_sim PRIVATE ${ORB_WARNINGS})
add_executable(orb_play_sim play_main.c sim_display.c sim_board.c)
target_link_libraries(orb_play_sim PRIVATE orb_games_view m)
target_compile_options(orb_play_sim PRIVATE ${ORB_WARNINGS})
add_test(NAME play_picker_default COMMAND orb_play_sim --index 0 --shot ${CMAKE_CURRENT_BINARY_DIR}/play-picker-default.png)
add_test(NAME play_picker_ax3_bold_rt COMMAND orb_play_sim --ax3 --bold --rt --index 4 --shot ${CMAKE_CURRENT_BINARY_DIR}/play-picker-ax3-bold-rt.png)
add_test(NAME game_beads_settled COMMAND orb_games_sim --game beads --headless
    --scene play --shot 4000=${CMAKE_CURRENT_BINARY_DIR}/game-beads-settled.png)
add_executable(test_games_render tests/test_games_render.c sim_display.c sim_board.c)
target_include_directories(test_games_render PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${COMPONENTS}/orb_games)
target_link_libraries(test_games_render PRIVATE orb_games_view)
target_compile_options(test_games_render PRIVATE ${ORB_WARNINGS})
add_test(NAME games_render COMMAND test_games_render ${CMAKE_CURRENT_BINARY_DIR})
add_executable(bench_games4 bench_games4.c sim_display.c sim_board.c)
target_link_libraries(bench_games4 PRIVATE orb_games_view)
target_compile_options(bench_games4 PRIVATE ${ORB_WARNINGS})

# Design 2.0 game chrome: real firmware views and process-local accessibility
# fixtures. Memory receipts use requested LVGL payload, never board heap claims.
add_executable(test_m_games_memory tests/test_m_games_memory.c emu_memory.c sim_display.c sim_board.c)
target_include_directories(test_m_games_memory PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_m_games_memory PRIVATE orb_games_view)
target_compile_options(test_m_games_memory PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_executable(test_m_games_layout tests/test_m_games_layout.c emu_memory.c sim_display.c sim_board.c)
target_include_directories(test_m_games_layout PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_m_games_layout PRIVATE orb_games_view)
target_compile_options(test_m_games_layout PRIVATE ${ORB_WARNINGS} -UNDEBUG)
foreach(mode default ax3 ax3-bold-rt)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/m-games-layout/${mode})
    add_test(NAME m_games_layout_${mode} COMMAND test_m_games_layout
        ${CMAKE_CURRENT_BINARY_DIR}/m-games-layout/${mode} ${mode})
    set_tests_properties(m_games_layout_${mode} PROPERTIES TIMEOUT 180)
endforeach()
set(ORB_GAMES_BASELINE_DIR "" CACHE PATH "Optional preserved pre-migration game sources for comparable receipts")
if(ORB_GAMES_BASELINE_DIR)
    add_executable(test_m_games_memory_baseline tests/test_m_games_memory.c emu_memory.c sim_display.c sim_board.c
        ${ORB_GAMES_BASELINE_DIR}/orb_game_view.c ${ORB_GAMES_BASELINE_DIR}/orb_game_play.c)
    target_include_directories(test_m_games_memory_baseline PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
        ${COMPONENTS}/orb_games/include)
    target_link_libraries(test_m_games_memory_baseline PRIVATE orb m)
    target_compile_options(test_m_games_memory_baseline PRIVATE ${ORB_WARNINGS} -UNDEBUG)
endif()
foreach(game marble beads orbit ring-snake pop)
    add_test(NAME game_${game}_default COMMAND orb_games_sim --game ${game} --headless
        --tilt 200=15,5 --shot 1500=${CMAKE_CURRENT_BINARY_DIR}/game-${game}-default.png)
    add_test(NAME game_${game}_ax3 COMMAND orb_games_sim --game ${game} --headless --text-size ax3
        --shot 1500=${CMAKE_CURRENT_BINARY_DIR}/game-${game}-ax3.png)
    add_test(NAME game_${game}_opaque COMMAND orb_games_sim --game ${game} --headless
        --reduce-transparency --shot 1500=${CMAKE_CURRENT_BINARY_DIR}/game-${game}-opaque.png)
    add_test(NAME game_${game}_ax3_bold_opaque COMMAND orb_games_sim --game ${game} --headless
        --text-size ax3 --bold --reduce-transparency --reduce-motion
        --shot 1500=${CMAKE_CURRENT_BINARY_DIR}/game-${game}-ax3-bold-opaque.png)
    add_test(NAME game_${game}_over COMMAND orb_games_sim --game ${game} --headless
        --game-over --text-size ax3 --shot 500=${CMAKE_CURRENT_BINARY_DIR}/game-${game}-over.png)
    foreach(scene start play paused end)
        foreach(size default ax3)
            add_test(NAME game_${game}_${scene}_${size} COMMAND orb_games_sim
                --game ${game} --headless --scene ${scene} --text-size ${size}
                --score 180 --best 120 --tilt 100=15,5
                --shot 600=${CMAKE_CURRENT_BINARY_DIR}/game-${game}-${scene}-${size}.png)
        endforeach()
    endforeach()
endforeach()

add_executable(test_game_logic ${COMPONENTS}/orb_games/tests/host/test_game_logic.c
    ${COMPONENTS}/orb_games/orb_game_logic.c
    ${COMPONENTS}/orb_games/orb_game_motion.c ${COMPONENTS}/orb_games/orb_game_record.c)
target_include_directories(test_game_logic PRIVATE ${COMPONENTS}/orb_games/include)
target_compile_options(test_game_logic PRIVATE ${ORB_WARNINGS})
target_link_libraries(test_game_logic PRIVATE m)
add_test(NAME game_logic COMMAND test_game_logic)
add_test(NAME beads_wall_exact COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_beads_wall_exact.py)
add_test(NAME beads_chain_exact COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_beads_chain_exact.py)
add_test(NAME beads_cell_exact COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_beads_chain_exact.py --cell)
add_executable(test_games4_other_play tests/test_games4_other_play.c
    ${COMPONENTS}/orb_games/orb_game_logic.c
    ${COMPONENTS}/orb_games/orb_game_motion.c ${COMPONENTS}/orb_games/orb_game_record.c)
target_include_directories(test_games4_other_play PRIVATE ${COMPONENTS}/orb_games/include)
target_compile_options(test_games4_other_play PRIVATE ${ORB_WARNINGS})
target_link_libraries(test_games4_other_play PRIVATE m)
add_test(NAME games4_other_play COMMAND test_games4_other_play)
add_executable(test_games4_snake_play tests/test_games4_snake_play.c
    ${COMPONENTS}/orb_games/orb_game_logic.c
    ${COMPONENTS}/orb_games/orb_game_motion.c ${COMPONENTS}/orb_games/orb_game_record.c)
target_include_directories(test_games4_snake_play PRIVATE ${COMPONENTS}/orb_games/include)
target_compile_options(test_games4_snake_play PRIVATE ${ORB_WARNINGS})
target_link_libraries(test_games4_snake_play PRIVATE m)
add_test(NAME games4_snake_play COMMAND test_games4_snake_play)
add_executable(test_games4_interpolate tests/test_games4_interpolate.c
    ${COMPONENTS}/orb_games/orb_game_logic.c
    ${COMPONENTS}/orb_games/orb_game_motion.c ${COMPONENTS}/orb_games/orb_game_record.c ${COMPONENTS}/orb_games/orb_game_interpolate.c)
target_include_directories(test_games4_interpolate PRIVATE ${COMPONENTS}/orb_games/include)
target_compile_options(test_games4_interpolate PRIVATE ${ORB_WARNINGS})
target_link_libraries(test_games4_interpolate PRIVATE m)
add_test(NAME games4_interpolate COMMAND test_games4_interpolate)

# Deterministic runtime fault tests isolate main/UI ownership and save failures
# from the separately linked native game renderer. Allocation wrappers verify
# that only entry allocates, while every tick/frame and queued input stays flat.
set(GAME_RUNTIME_HOST ${COMPONENTS}/orb_games/tests/host)
set(GAME_RUNTIME_INCLUDES
    ${GAME_RUNTIME_HOST}/runtime_stubs
    ${COMPONENTS}/orb_games/include
    ${COMPONENTS}/orb_apps/include
    ${COMPONENTS}/orb_sound/include
    ${COMPONENTS}/orb_fs/include
    ${COMPONENTS}/orb_fs/tests/host)
add_library(game_runtime_test_object OBJECT ${COMPONENTS}/orb_games/orb_games.c)
target_include_directories(game_runtime_test_object PRIVATE ${GAME_RUNTIME_INCLUDES})
target_compile_definitions(game_runtime_test_object PRIVATE calloc=orb_test_calloc free=orb_test_free)
target_link_libraries(game_runtime_test_object PRIVATE orb_tokens)
target_compile_options(game_runtime_test_object PRIVATE ${ORB_WARNINGS})
# Runtime fault injection keeps fake stepping, but tests the production garden
# codec. Rename only the four lifecycle symbols supplied by that fake.
add_library(game_record_test_object OBJECT ${COMPONENTS}/orb_games/orb_game_logic.c
    ${COMPONENTS}/orb_games/orb_game_motion.c ${COMPONENTS}/orb_games/orb_game_record.c)
target_include_directories(game_record_test_object PRIVATE ${COMPONENTS}/orb_games/include)
target_compile_definitions(game_record_test_object PRIVATE
    orb_game_id=orb_test_real_id orb_game_init=orb_test_real_init
    orb_game_step=orb_test_real_step orb_game_action=orb_test_real_action)
target_compile_options(game_record_test_object PRIVATE ${ORB_WARNINGS})
add_executable(test_game_runtime ${GAME_RUNTIME_HOST}/test_game_runtime.c
    ${COMPONENTS}/orb_games/orb_game_interpolate.c
    ${COMPONENTS}/orb_apps/orb_apps.c ${COMPONENTS}/orb_apps/orb_marble.c
    $<TARGET_OBJECTS:game_runtime_test_object> $<TARGET_OBJECTS:game_record_test_object>)
target_include_directories(test_game_runtime PRIVATE ${GAME_RUNTIME_INCLUDES})
target_link_libraries(test_game_runtime PRIVATE orb_tokens)
target_compile_options(test_game_runtime PRIVATE ${ORB_WARNINGS})
target_link_libraries(test_game_runtime PRIVATE m)
add_test(NAME game_runtime COMMAND test_game_runtime)

# Home uses the firmware view and bounded store; pointer sequences never contact a hub.
set(ORB_CJSON_DIR "${FIRMWARE}/managed_components/espressif__cjson/cJSON" CACHE PATH
    "ESP-IDF cJSON sources (may point to an existing managed-component checkout)")
add_library(home_wire_fixture STATIC
    ${COMPONENTS}/orb_link/link_home.c
    ${ORB_CJSON_DIR}/cJSON.c)
target_include_directories(home_wire_fixture PUBLIC ${COMPONENTS}/orb_link
    ${COMPONENTS}/orb_home_data/include
    ${COMPONENTS}/orb_home/include ${COMPONENTS}/orb_model/include
    ${ORB_CJSON_DIR})
target_compile_definitions(home_wire_fixture PUBLIC
    HOME_FIXTURE_DIR="${COMPONENTS}/orb_home/tests/fixtures")
add_executable(orb_home_sim home_main.c home_memory.c sim_display.c sim_board.c)
target_include_directories(orb_home_sim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(orb_home_sim PRIVATE orb home_wire_fixture m)
target_compile_options(orb_home_sim PRIVATE ${ORB_WARNINGS})
add_executable(test_home_view tests/test_home_view.c emu_memory.c sim_display.c sim_board.c)
target_include_directories(test_home_view PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_home_view PRIVATE orb home_wire_fixture m)
target_compile_options(test_home_view PRIVATE ${ORB_WARNINGS})
add_test(NAME home_view COMMAND test_home_view)
add_executable(test_home_board tests/test_home_board.c sim_display.c sim_board.c)
target_include_directories(test_home_board PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_home_board PRIVATE orb home_wire_fixture m)
target_compile_options(test_home_board PRIVATE ${ORB_WARNINGS})
add_test(NAME home_board COMMAND test_home_board)
add_test(NAME home_board_ax3 COMMAND test_home_board ax3)
add_test(NAME home_board_ax3_bold_rt COMMAND test_home_board ax3 bold rt)
add_test(NAME home_board_reduce_motion COMMAND test_home_board ax3 bold rt rm)
foreach(scene home morning evening night rooms rooms-next room-living room-living-next room-bedroom room-kitchen room-office room-dining climate light light-off light-state-cycle colour white scenes thermostat modes fan lock unlocked switch outlet temperature humidity motion low-battery contact wire-temperature wire-humidity wire-motion wire-contact no-response offline unavailable unavailable-lock waiting unknown-lock error hub-home hub-rooms hub-room hub-climate hub-light hub-colour hub-lock hub-fan no-favourites lights-0 lights-1 lights-2 switch-light-summary offline-lock offline-unlocked long-scene wide-scene long-error wide-error offline-climate offline-rooms offline-sensors
    empty-home loading-home busy-rooms rooms-scrolled busy-room busy-room-scrolled busy-room-last empty-room room-all-off-row room-actions all-off-pending all-off-accepted all-off-failed
    pending-light pending-switch pending-room pending-lock scene-pending scene-accepted scene-failed
    capsule-pending capsule-accepted capsule-failed thermostat-f-heat thermostat-f-cool thermostat-f-auto
    thermostat-f-off thermostat-f-modes sensor-aged sensor-unknown-age sensor-future-age
    rooms-list-top rooms-list-off rooms-list-offline rooms-list-pending rooms-list-failed rooms-list-scenes
    rooms-list-end rooms-list-stale rooms-list-empty)
    add_test(NAME home_${scene}_default COMMAND orb_home_sim ${scene} ${CMAKE_CURRENT_BINARY_DIR}/home-${scene}-default.png)
    add_test(NAME home_${scene}_ax3 COMMAND orb_home_sim ${scene} ${CMAKE_CURRENT_BINARY_DIR}/home-${scene}-ax3.png ax3)
    add_test(NAME home_${scene}_ax3_bold_rt COMMAND orb_home_sim ${scene} ${CMAKE_CURRENT_BINARY_DIR}/home-${scene}-ax3-bold-rt.png ax3 bold rt)
endforeach()

add_test(NAME home_view_larger COMMAND test_home_view larger)
add_test(NAME home_view_ax3_bold_rt COMMAND test_home_view ax3 bold rt)
add_test(NAME home_view_reduce_motion COMMAND test_home_view rm)
add_test(NAME home_view_ax3_bold_rt_reduce_motion COMMAND test_home_view ax3 bold rt rm)
foreach(scene home-app home-controls home-face home-stack home-doorbell home-controls-unavailable home-controls-waiting)
    add_test(NAME shell_${scene}_default COMMAND orb_shell_sim ${scene} ${CMAKE_CURRENT_BINARY_DIR}/shell-${scene}-default.png)
    add_test(NAME shell_${scene}_ax3 COMMAND orb_shell_sim ${scene} ${CMAKE_CURRENT_BINARY_DIR}/shell-${scene}-ax3.png ax3)
    add_test(NAME shell_${scene}_ax3_bold_rt COMMAND orb_shell_sim ${scene} ${CMAKE_CURRENT_BINARY_DIR}/shell-${scene}-ax3-bold-rt.png ax3 bold rt)
endforeach()

add_executable(test_home_shell tests/test_home_shell.c sim_display.c sim_board.c)
target_include_directories(test_home_shell PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_home_shell PRIVATE orb m)
target_compile_options(test_home_shell PRIVATE ${ORB_WARNINGS})
add_test(NAME home_shell_integration COMMAND test_home_shell)
add_test(NAME home_shell_ax3_bold_rt COMMAND test_home_shell ax3 bold rt)

add_executable(test_scroll_fixtures tests/test_scroll_fixtures.c sim_display.c sim_board.c)
target_include_directories(test_scroll_fixtures PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${COMPONENTS}/orb_devtools)
target_link_libraries(test_scroll_fixtures PRIVATE orb m)
target_compile_options(test_scroll_fixtures PRIVATE ${ORB_WARNINGS})
add_test(NAME scroll_fixtures COMMAND test_scroll_fixtures)

# Navigation is portable; these independent oracles also run without LVGL.
add_executable(test_nav ${COMPONENTS}/orb_nav/tests/test_nav.c ${COMPONENTS}/orb_nav/orb_nav.c)
target_include_directories(test_nav PRIVATE ${COMPONENTS}/orb_nav/include)
target_compile_options(test_nav PRIVATE ${ORB_WARNINGS} -UNDEBUG)
target_link_libraries(test_nav PRIVATE m)
add_test(NAME nav_core COMMAND test_nav)
add_executable(test_nav_compositor ${COMPONENTS}/orb_nav/tests/test_compositor.c ${COMPONENTS}/orb_nav/orb_nav_compositor.c
    ${COMPONENTS}/orb_nav/orb_nav_compositor_slots.c)
target_include_directories(test_nav_compositor PRIVATE ${COMPONENTS}/orb_nav/include)
target_compile_options(test_nav_compositor PRIVATE ${ORB_WARNINGS})
target_link_libraries(test_nav_compositor PRIVATE m)
add_test(NAME nav_compositor COMMAND test_nav_compositor)
add_executable(test_nav_iris_oracle ${COMPONENTS}/orb_nav/tests/test_iris_oracle.c ${COMPONENTS}/orb_nav/orb_nav_compositor.c
    ${COMPONENTS}/orb_nav/orb_nav_compositor_slots.c)
target_include_directories(test_nav_iris_oracle PRIVATE ${COMPONENTS}/orb_nav/include)
target_include_directories(test_nav_iris_oracle PRIVATE ${COMPONENTS}/orb_lv_simd/include)
target_compile_definitions(test_nav_iris_oracle PRIVATE __XTENSA__ ORB_TEST_S3_COPY)
target_compile_options(test_nav_iris_oracle PRIVATE ${ORB_WARNINGS})
target_link_libraries(test_nav_iris_oracle PRIVATE m)
add_test(NAME nav_iris_oracle COMMAND test_nav_iris_oracle)
add_executable(test_nav_simd_copy ${COMPONENTS}/orb_nav/tests/test_simd_copy.c ${COMPONENTS}/orb_lv_simd/src/orb_lv_blend.c)
target_include_directories(test_nav_simd_copy PRIVATE ${COMPONENTS}/orb_lv_simd/include)
target_include_directories(test_nav_simd_copy SYSTEM PRIVATE ${LVGL_DIR}/src)
target_compile_options(test_nav_simd_copy PRIVATE ${ORB_WARNINGS})
target_link_libraries(test_nav_simd_copy PRIVATE lvgl m)
add_test(NAME nav_simd_copy COMMAND test_nav_simd_copy)
add_executable(test_nav_compositor_work ${COMPONENTS}/orb_nav/tests/test_compositor_work.c ${COMPONENTS}/orb_nav/orb_nav_compositor.c
    ${COMPONENTS}/orb_nav/orb_nav_compositor_slots.c)
target_include_directories(test_nav_compositor_work PRIVATE ${COMPONENTS}/orb_nav/include ${COMPONENTS}/orb_lv_simd/include)
target_compile_definitions(test_nav_compositor_work PRIVATE __XTENSA__ _FORTIFY_SOURCE=0 memcpy=orb_nav_test_memcpy)
target_compile_options(test_nav_compositor_work PRIVATE ${ORB_WARNINGS} -fno-builtin-memcpy)
target_link_libraries(test_nav_compositor_work PRIVATE m)
add_test(NAME nav_compositor_work COMMAND test_nav_compositor_work)
add_executable(test_nav_compositor_esp_allocator
    ${COMPONENTS}/orb_nav/tests/test_compositor_esp_allocator.c
    ${COMPONENTS}/orb_nav/orb_nav_compositor.c
    ${COMPONENTS}/orb_nav/orb_nav_compositor_slots.c)
target_include_directories(test_nav_compositor_esp_allocator PRIVATE
    ${COMPONENTS}/orb_nav/include ${COMPONENTS}/orb_nav/tests/stubs ${COMPONENTS}/orb_memory/include)
target_compile_definitions(test_nav_compositor_esp_allocator PRIVATE ESP_PLATFORM)
target_compile_options(test_nav_compositor_esp_allocator PRIVATE ${ORB_WARNINGS})
target_link_libraries(test_nav_compositor_esp_allocator PRIVATE m)
add_test(NAME nav_compositor_esp_allocator COMMAND test_nav_compositor_esp_allocator)
add_executable(test_nav_admission ${COMPONENTS}/orb_nav/tests/test_admission.c ${COMPONENTS}/orb_nav/orb_nav_admission.c)
target_include_directories(test_nav_admission PRIVATE ${COMPONENTS}/orb_nav/include)
target_compile_options(test_nav_admission PRIVATE ${ORB_WARNINGS})
add_test(NAME nav_admission COMMAND test_nav_admission)

add_executable(test_nav_shell tests/test_nav_shell.c sim_display.c sim_board.c)
target_include_directories(test_nav_shell PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_nav_shell PRIVATE orb m)
target_compile_options(test_nav_shell PRIVATE ${ORB_WARNINGS})
add_test(NAME nav_shell COMMAND test_nav_shell)

add_executable(test_neighbour tests/test_neighbour.c sim_display.c sim_board.c)
target_include_directories(test_neighbour PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${COMPONENTS}/orb_shell)
target_link_libraries(test_neighbour PRIVATE orb m)
target_compile_options(test_neighbour PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME neighbour_scenes COMMAND test_neighbour)
add_executable(test_gestures3 tests/test_gestures3.c sim_display.c sim_board.c)
target_include_directories(test_gestures3 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_gestures3 PRIVATE orb m)
target_compile_options(test_gestures3 PRIVATE ${ORB_WARNINGS})
add_test(NAME gestures3 COMMAND test_gestures3)
add_executable(test_gestures4_nav tests/test_gestures4_nav.c sim_display.c sim_board.c)
target_include_directories(test_gestures4_nav PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_gestures4_nav PRIVATE orb m)
target_compile_options(test_gestures4_nav PRIVATE ${ORB_WARNINGS})
add_test(NAME gestures4_nav COMMAND test_gestures4_nav)
add_test(NAME qa_nav_ax3_bold_rt COMMAND test_gestures4_nav ax3 qa)
add_test(NAME qa_nav_reduce_motion COMMAND test_gestures4_nav default qa rm)
add_executable(test_devtools_input tests/test_devtools_input.c sim_display.c sim_board.c
    ${ORB_CJSON_DIR}/cJSON.c)
target_include_directories(test_devtools_input PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    ${COMPONENTS}/orb_devtools ${ORB_CJSON_DIR})
target_link_libraries(test_devtools_input PRIVATE orb m)
target_compile_options(test_devtools_input PRIVATE ${ORB_WARNINGS})
add_test(NAME devtools_input COMMAND test_devtools_input)
add_executable(test_list_scroll tests/test_list_scroll.c sim_display.c sim_board.c)
target_include_directories(test_list_scroll PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_list_scroll PRIVATE orb m)
target_compile_options(test_list_scroll PRIVATE ${ORB_WARNINGS})
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/list-scroll/default ${CMAKE_CURRENT_BINARY_DIR}/list-scroll/ax3)
add_test(NAME list_scroll COMMAND test_list_scroll default ${CMAKE_CURRENT_BINARY_DIR}/list-scroll/default)
add_test(NAME list_scroll_ax3 COMMAND test_list_scroll ax3 ${CMAKE_CURRENT_BINARY_DIR}/list-scroll/ax3)
add_executable(test_gesture_controls tests/test_gesture_controls.c sim_display.c sim_board.c)
target_include_directories(test_gesture_controls PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_gesture_controls PRIVATE orb m)
target_compile_options(test_gesture_controls PRIVATE ${ORB_WARNINGS} -UNDEBUG)
foreach(mode default ax3-bold-rt)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/gesture-controls/${mode})
    set(CONTROL_FLAGS)
    if(mode STREQUAL "ax3-bold-rt")
        list(APPEND CONTROL_FLAGS ax3)
    endif()
    add_test(NAME gesture_controls_${mode} COMMAND test_gesture_controls
        ${CMAKE_CURRENT_BINARY_DIR}/gesture-controls/${mode} ${CONTROL_FLAGS})
endforeach()
foreach(variant default ax3-bold-rt reduce-motion)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/nav-${variant})
endforeach()
add_test(NAME nav_gestures_default COMMAND orb_shell_sim --nav ${CMAKE_CURRENT_BINARY_DIR}/nav-default)
add_test(NAME nav_gestures_ax3_bold_rt COMMAND orb_shell_sim --nav ${CMAKE_CURRENT_BINARY_DIR}/nav-ax3-bold-rt ax3 bold rt)
add_test(NAME nav_gestures_reduce_motion COMMAND orb_shell_sim --nav ${CMAKE_CURRENT_BINARY_DIR}/nav-reduce-motion rm)

# Stream C: isolated public-data face stub; the shell branch consumes the same
# controller later. These fixtures never link the board adapter or touch hardware.
add_executable(orb_power_sim power_main.c power_view.c sim_display.c sim_board.c)
target_include_directories(orb_power_sim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    ${COMPONENTS}/orb_power/include)
target_link_libraries(orb_power_sim PRIVATE orb m)
target_compile_options(orb_power_sim PRIVATE ${ORB_WARNINGS})
add_test(NAME power_preview_default COMMAND orb_power_sim ${CMAKE_CURRENT_BINARY_DIR}/power-default)
add_test(NAME power_preview_ax3_bold_rt COMMAND orb_power_sim ${CMAKE_CURRENT_BINARY_DIR}/power-ax3-bold-rt ax3 bold rt)
add_test(NAME power_preview_reduce_motion COMMAND orb_power_sim ${CMAKE_CURRENT_BINARY_DIR}/power-reduce-motion rm)
add_test(NAME power_preview_ax3_bold_rt_rm COMMAND orb_power_sim ${CMAKE_CURRENT_BINARY_DIR}/power-ax3-bold-rt-rm ax3 bold rt rm)
add_test(NAME power_preview_increase_contrast COMMAND orb_power_sim ${CMAKE_CURRENT_BINARY_DIR}/power-increase-contrast ax3 bold rt ic)

add_executable(test_raster_budget tests/test_raster_budget.c sim_display.c sim_board.c)
target_include_directories(test_raster_budget PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_raster_budget PRIVATE orb m)
target_compile_options(test_raster_budget PRIVATE ${ORB_WARNINGS})
add_test(NAME raster_budget COMMAND test_raster_budget)

# Stream F: real Time views/registry over fake monotonic clocks and pointers.
add_executable(test_time_core ${COMPONENTS}/orb_time/orb_time.c
    ${COMPONENTS}/orb_time/orb_time_clock.c ${COMPONENTS}/orb_time/orb_time_record.c
    ${COMPONENTS}/orb_time_app/orb_time.c
    ${COMPONENTS}/orb_time_app/tests/host/test_time.c
    ${COMPONENTS}/orb_apps/orb_apps.c ${COMPONENTS}/orb_apps/orb_marble.c)
target_include_directories(test_time_core PRIVATE ${COMPONENTS}/orb_time/include
    ${COMPONENTS}/orb_apps/include ${COMPONENTS}/orb_sound/include
    ${COMPONENTS}/orb_fs/include ${COMPONENTS}/orb_fs/tests/host)
target_compile_definitions(test_time_core PRIVATE _POSIX_C_SOURCE=200809L ORB_TIME_TEST_STORAGE=1)
target_link_libraries(test_time_core PRIVATE orb_tokens)
target_compile_options(test_time_core PRIVATE ${ORB_WARNINGS} -UNDEBUG)
target_link_libraries(test_time_core PRIVATE m)
add_test(NAME time_core COMMAND test_time_core)
set_tests_properties(time_core PROPERTIES ENVIRONMENT "TZ=UTC0")

add_executable(orb_time_sim time_main.c emu_memory.c sim_display.c sim_board.c)
target_link_libraries(orb_time_sim PRIVATE orb m)
target_compile_options(orb_time_sim PRIVATE ${ORB_WARNINGS})
add_executable(orb_time_shell_sim time_shell_main.c sim_display.c sim_board.c)
target_link_libraries(orb_time_shell_sim PRIVATE orb m)
target_compile_options(orb_time_shell_sim PRIVATE ${ORB_WARNINGS})
foreach(mode default ax3 ax3-bold-rt ax3-bold-rt-rm)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/time-${mode}
        ${CMAKE_CURRENT_BINARY_DIR}/time-shell-${mode})
endforeach()
add_test(NAME time_default COMMAND orb_time_sim ${CMAKE_CURRENT_BINARY_DIR}/time-default)
add_test(NAME time_ax3 COMMAND orb_time_sim ${CMAKE_CURRENT_BINARY_DIR}/time-ax3 ax3)
add_test(NAME time_ax3_bold_rt COMMAND orb_time_sim ${CMAKE_CURRENT_BINARY_DIR}/time-ax3-bold-rt ax3 bold rt)
add_test(NAME time_shell_default COMMAND orb_time_shell_sim ${CMAKE_CURRENT_BINARY_DIR}/time-shell-default)
add_test(NAME time_shell_ax3 COMMAND orb_time_shell_sim ${CMAKE_CURRENT_BINARY_DIR}/time-shell-ax3 ax3)
add_test(NAME time_shell_ax3_bold_rt COMMAND orb_time_shell_sim ${CMAKE_CURRENT_BINARY_DIR}/time-shell-ax3-bold-rt ax3 bold rt)
add_test(NAME time_ax3_bold_rt_rm COMMAND orb_time_sim ${CMAKE_CURRENT_BINARY_DIR}/time-ax3-bold-rt-rm ax3 bold rt rm)
add_test(NAME time_shell_ax3_bold_rt_rm COMMAND orb_time_shell_sim ${CMAKE_CURRENT_BINARY_DIR}/time-shell-ax3-bold-rt-rm ax3 bold rt rm)
add_executable(test_time_voice tests/test_time_voice.c)
target_link_libraries(test_time_voice PRIVATE orb m)
target_compile_options(test_time_voice PRIVATE ${ORB_WARNINGS})
add_test(NAME time_voice COMMAND test_time_voice)

# Time's Design 2.0 contracts inspect the actual production view. Keep the
# strict inset-title requirement visible until orb_ui gains the requested API.
add_executable(test_time_design2 tests/test_time_design2.c sim_display.c sim_board.c)
target_include_directories(test_time_design2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_time_design2 PRIVATE orb m)
target_compile_options(test_time_design2 PRIVATE ${ORB_WARNINGS})
add_test(NAME time_design2_default COMMAND test_time_design2)
add_test(NAME time_design2_ax3 COMMAND test_time_design2 ax3)
add_test(NAME time_design2_ax3_bold_rt COMMAND test_time_design2 ax3 bold rt)
add_test(NAME time_design2_inset COMMAND test_time_design2 inset)
add_test(NAME time_design2_inset_ax3 COMMAND test_time_design2 inset ax3)
add_test(NAME time_design2_inset_ax3_bold_rt COMMAND test_time_design2 inset ax3 bold rt)

add_executable(test_time_steady tests/test_time_steady.c sim_display.c sim_board.c)
target_include_directories(test_time_steady PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_time_steady PRIVATE orb m)
target_compile_options(test_time_steady PRIVATE ${ORB_WARNINGS})
foreach(mode default ax3 rt)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/time-steady/${mode})
    foreach(case paging transport)
        add_test(NAME time_steady_${case}_${mode} COMMAND test_time_steady ${case}
            ${CMAKE_CURRENT_BINARY_DIR}/time-steady/${mode} ${mode})
    endforeach()
endforeach()

# The real stopwatch viewport must admit only visible kit rows. Instrument
# component allocation separately from LVGL's documented render-task storage.
add_executable(test_time_cache2 tests/test_time_cache2.c emu_memory.c sim_display.c sim_board.c)
target_include_directories(test_time_cache2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_time_cache2 PRIVATE orb m)
target_compile_options(test_time_cache2 PRIVATE ${ORB_WARNINGS})
add_test(NAME time_cache2_default COMMAND test_time_cache2)
add_test(NAME time_cache2_ax3 COMMAND test_time_cache2 ax3)
add_test(NAME time_cache2_ax3_bold_rt COMMAND test_time_cache2 ax3-bold-rt)

# Render actual Time persistence failures. The executable provides the storage-
# enabled service before liborb, whose ordinary no-storage service stays unused.
add_executable(orb_time_storage_sim time_storage_main.c
    ${COMPONENTS}/orb_time/orb_time.c ${COMPONENTS}/orb_time/orb_time_clock.c
    ${COMPONENTS}/orb_time/orb_time_record.c sim_display.c sim_board.c)
target_include_directories(orb_time_storage_sim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(orb_time_storage_sim PRIVATE ORB_TIME_TEST_STORAGE=1)
target_link_libraries(orb_time_storage_sim PRIVATE orb m)
target_compile_options(orb_time_storage_sim PRIVATE ${ORB_WARNINGS})
add_test(NAME time_storage_default COMMAND orb_time_storage_sim ${CMAKE_CURRENT_BINARY_DIR}/time-storage-default)
add_test(NAME time_storage_ax3 COMMAND orb_time_storage_sim ${CMAKE_CURRENT_BINARY_DIR}/time-storage-ax3 ax3)
add_test(NAME time_storage_ax3_bold_rt COMMAND orb_time_storage_sim ${CMAKE_CURRENT_BINARY_DIR}/time-storage-ax3-bold-rt ax3 bold rt)
add_test(NAME time_storage_ax3_bold_rt_rm COMMAND orb_time_storage_sim ${CMAKE_CURRENT_BINARY_DIR}/time-storage-ax3-bold-rt-rm ax3 bold rt rm)

# The physical recognizer is the same allocation-free source used by main.
add_executable(test_buttons ${COMPONENTS}/orb_buttons/orb_buttons.c
    ${COMPONENTS}/orb_buttons/tests/host/test_buttons.c)
target_include_directories(test_buttons PRIVATE ${COMPONENTS}/orb_buttons/include)
target_compile_options(test_buttons PRIVATE ${ORB_WARNINGS})
add_test(NAME buttons_core COMMAND test_buttons)

# Owner-reported Face and physical-button regressions, no device access.
add_executable(test_face_layout tests/test_face_layout.c sim_display.c sim_board.c)
target_include_directories(test_face_layout PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_face_layout PRIVATE orb m)
target_compile_options(test_face_layout PRIVATE ${ORB_WARNINGS})
add_test(NAME face_layout COMMAND test_face_layout)
add_executable(test_buttons_shell tests/test_buttons_shell.c sim_display.c sim_board.c
    ${COMPONENTS}/orb_games/orb_games.c)
target_include_directories(test_buttons_shell PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_buttons_shell PRIVATE orb_games_view)
target_compile_options(test_buttons_shell PRIVATE ${ORB_WARNINGS})
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/owner-fixes/default ${CMAKE_CURRENT_BINARY_DIR}/owner-fixes/ax3-bold-rt)
add_test(NAME buttons_shell_default COMMAND test_buttons_shell ${CMAKE_CURRENT_BINARY_DIR}/owner-fixes/default)
add_test(NAME buttons_shell_ax3_bold_rt COMMAND test_buttons_shell ${CMAKE_CURRENT_BINARY_DIR}/owner-fixes/ax3-bold-rt ax3)

# Interactive firmware shell. The CLIB allocator provider is linked directly:
# only this executable gets LVGL memory accounting; image tests keep their heap.
add_executable(orb_emu emu_main.c emu_input.c emu_fixture.c emu_memory.c
    sim_display.c sim_board.c
    ${COMPONENTS}/orb_games/orb_games.c ${COMPONENTS}/orb_voice/voice_core.c)
target_include_directories(orb_emu PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    ${COMPONENTS}/orb_voice)
target_compile_definitions(orb_emu PRIVATE
    ORB_EMU_SHOT_DIR="${CMAKE_CURRENT_BINARY_DIR}/emu-shots")
target_link_libraries(orb_emu PRIVATE orb_games_view SDL2::SDL2 m)
target_compile_options(orb_emu PRIVATE ${ORB_WARNINGS})
add_test(NAME emu_smoke COMMAND orb_emu --headless-frames 6
    --out ${CMAKE_CURRENT_BINARY_DIR}/emu-smoke)
add_test(NAME emu_smoke_2x COMMAND orb_emu --scale 2 --headless-frames 24
    --diagnostics --out ${CMAKE_CURRENT_BINARY_DIR}/emu-smoke-2x)
set_tests_properties(emu_smoke emu_smoke_2x PROPERTIES
    ENVIRONMENT "SDL_VIDEODRIVER=dummy" TIMEOUT 30)

add_executable(test_emu_input tests/test_emu_input.c emu_input.c sim_display.c sim_board.c)
target_include_directories(test_emu_input PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_emu_input PRIVATE orb SDL2::SDL2 m)
target_compile_options(test_emu_input PRIVATE ${ORB_WARNINGS})
add_test(NAME emu_input COMMAND test_emu_input)
add_executable(test_emu_memory tests/test_emu_memory.c emu_memory.c)
target_link_libraries(test_emu_memory PRIVATE lvgl pthread m)
target_compile_options(test_emu_memory PRIVATE ${ORB_WARNINGS})
add_test(NAME emu_memory COMMAND test_emu_memory)

add_executable(test_emu_flow tests/test_emu_flow.c emu_fixture.c emu_input.c emu_memory.c
    sim_display.c sim_board.c ${COMPONENTS}/orb_games/orb_games.c
    ${COMPONENTS}/orb_voice/voice_core.c)
target_include_directories(test_emu_flow PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    ${COMPONENTS}/orb_voice)
target_link_libraries(test_emu_flow PRIVATE orb_games_view SDL2::SDL2 m)
target_compile_options(test_emu_flow PRIVATE ${ORB_WARNINGS})
add_test(NAME emu_flow COMMAND test_emu_flow)
set_tests_properties(emu_flow PROPERTIES TIMEOUT 60)


# Design 1.0: exact shell geometry and the real SDL presentation of each frame.
# Each process starts with isolated synthetic content and no persistent settings.
add_executable(test_design1 tests/test_design1.c emu_fixture.c emu_input.c emu_memory.c
    sim_display.c sim_board.c ${COMPONENTS}/orb_games/orb_games.c
    ${COMPONENTS}/orb_voice/voice_core.c)
target_include_directories(test_design1 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${COMPONENTS}/orb_voice)
target_link_libraries(test_design1 PRIVATE orb_games_view SDL2::SDL2 m)
target_compile_options(test_design1 PRIVATE ${ORB_WARNINGS})
foreach(mode default ax3-bold-rt reduce-motion increase-contrast)
    add_test(NAME design1_geometry_${mode} COMMAND test_design1 ${mode})
endforeach()
foreach(scene Face FaceWaiting AlwaysOn Stack Notifications NotificationDetail Offline Charging
    Apps ControlCenter Compass PowerSheet Settings Setup Updating)
    foreach(mode default ax3-bold-rt)
        set(DESIGN1_FLAGS)
        if(mode STREQUAL "ax3-bold-rt")
            list(APPEND DESIGN1_FLAGS --text-size ax3 --bold --reduce-transparency)
        endif()
        add_test(NAME design1_${scene}_${mode} COMMAND orb_emu --design-scene ${scene}
            --headless-frames 60 --out ${CMAKE_CURRENT_BINARY_DIR}/design1/${scene}-${mode} ${DESIGN1_FLAGS})
        set_tests_properties(design1_${scene}_${mode} PROPERTIES TIMEOUT 30 ENVIRONMENT "SDL_VIDEODRIVER=dummy")
    endforeach()
endforeach()
add_executable(test_shell_power tests/test_shell_power.c sim_display.c sim_board.c)
target_include_directories(test_shell_power PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_shell_power PRIVATE orb m)
target_compile_options(test_shell_power PRIVATE ${ORB_WARNINGS})
add_test(NAME shell_power COMMAND test_shell_power)
add_test(NAME shell_power_ax3_bold_rt_rm COMMAND test_shell_power ax3)

# Round-first list presentation: geometry and cached sources under scroll.
add_executable(test_chord_list tests/test_chord_list.c sim_display.c sim_board.c)
target_include_directories(test_chord_list PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_chord_list PRIVATE orb m)
target_compile_options(test_chord_list PRIVATE ${ORB_WARNINGS})
add_test(NAME chord_list COMMAND test_chord_list)

# Design 1.0 Agents use the real bounded protocol decoder and retained firmware
# views, with isolated fake provider traffic and physical/synthetic pointers.
add_library(agents_wire_fixture STATIC ${COMPONENTS}/orb_link/link_v11.c)
target_include_directories(agents_wire_fixture PUBLIC ${COMPONENTS}/orb_link)
target_link_libraries(agents_wire_fixture PUBLIC orb home_wire_fixture m)
target_compile_options(agents_wire_fixture PRIVATE ${ORB_WARNINGS})
add_executable(orb_agents_sim agents_main.c sim_display.c sim_board.c)
target_include_directories(orb_agents_sim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(orb_agents_sim PRIVATE agents_wire_fixture)
target_compile_options(orb_agents_sim PRIVATE ${ORB_WARNINGS})
foreach(mode default ax3-bold-rt)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/agents-${mode})
    add_test(NAME agents_design1_${mode} COMMAND orb_agents_sim
        ${CMAKE_CURRENT_BINARY_DIR}/agents-${mode} ${mode})
    set_tests_properties(agents_design1_${mode} PROPERTIES TIMEOUT 90)
endforeach()

add_executable(test_notifications tests/test_notifications.c sim_display.c sim_board.c)
target_include_directories(test_notifications PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_notifications PRIVATE orb m)
target_compile_options(test_notifications PRIVATE ${ORB_WARNINGS})
foreach(mode default ax3-bold-rt)
    add_test(NAME notifications_sources_${mode} COMMAND test_notifications ${mode}
        ${CMAKE_CURRENT_BINARY_DIR}/notifications-sources-${mode}.png)
endforeach()

# Design 1.0 sound/system device surfaces; process-local fixtures only.
add_executable(orb_nowplaying_sim nowplaying_main.c emu_memory.c sim_display.c sim_board.c)
target_include_directories(orb_nowplaying_sim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(orb_nowplaying_sim PRIVATE agents_wire_fixture orb m)
target_compile_options(orb_nowplaying_sim PRIVATE ${ORB_WARNINGS})
add_executable(test_system_state tests/test_system_state.c ${COMPONENTS}/orb_shell/orb_system_state.c)
target_include_directories(test_system_state PRIVATE ${COMPONENTS}/orb_shell/include
    ${COMPONENTS}/orb_fs/include ${COMPONENTS}/orb_fs/tests/host)
target_compile_definitions(test_system_state PRIVATE ORB_SYSTEM_TEST_STORAGE)
target_compile_options(test_system_state PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME system_state COMMAND test_system_state)
foreach(mode default ax3-bold-rt)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/sound-ui/${mode})
    add_test(NAME nowplaying_design1_${mode} COMMAND orb_nowplaying_sim
        ${CMAKE_CURRENT_BINARY_DIR}/sound-ui/${mode} ${mode})
endforeach()
add_executable(test_system_views tests/test_system_views.c emu_memory.c sim_display.c sim_board.c)
target_include_directories(test_system_views PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_system_views PRIVATE orb m)
target_compile_options(test_system_views PRIVATE ${ORB_WARNINGS})
foreach(mode default ax3 ax3-bold-rt)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/sound-ui/${mode})
    add_test(NAME system_views_${mode} COMMAND test_system_views ${mode}
        ${CMAKE_CURRENT_BINARY_DIR}/sound-ui/${mode})
endforeach()
add_executable(orb_sound_sim sound_main.c emu_memory.c sim_display.c sim_board.c)
target_include_directories(orb_sound_sim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${COMPONENTS}/orb_shell)
target_link_libraries(orb_sound_sim PRIVATE orb m)
target_compile_options(orb_sound_sim PRIVATE ${ORB_WARNINGS})
foreach(mode default ax3 ax3-bold-rt)
    add_test(NAME sound_design1_${mode} COMMAND orb_sound_sim
        ${CMAKE_CURRENT_BINARY_DIR}/sound-ui/${mode} ${mode})
endforeach()

foreach(scene SoundSettings VolumeHUD SpokenReply AlarmRinging NowPlaying Battery Onboarding)
    foreach(mode default ax3-bold-rt)
        set(SOUND_FLAGS)
        if(mode STREQUAL "ax3-bold-rt")
            list(APPEND SOUND_FLAGS --text-size ax3 --bold --reduce-transparency)
        endif()
        add_test(NAME sound_emu_${scene}_${mode} COMMAND orb_emu --design-scene ${scene}
            --headless-frames 60 --out ${CMAKE_CURRENT_BINARY_DIR}/sound-ui/emu/${scene}-${mode} ${SOUND_FLAGS})
        set_tests_properties(sound_emu_${scene}_${mode} PROPERTIES TIMEOUT 30 ENVIRONMENT "SDL_VIDEODRIVER=dummy")
    endforeach()
endforeach()

# The lead's tilt override shares one parser/expiry state machine with firmware.
# Keep its expiry and force bounds in the normal simulator regression gate.
add_executable(test_devtools_tilt ${COMPONENTS}/orb_devtools/tests/host/test_tilt_command.c)
target_include_directories(test_devtools_tilt PRIVATE ${COMPONENTS}/orb_devtools)
target_link_libraries(test_devtools_tilt PRIVATE m)
target_compile_options(test_devtools_tilt PRIVATE ${ORB_WARNINGS})
add_test(NAME devtools_tilt COMMAND test_devtools_tilt)

# Resting-pose regressions use the real motion filter and game integrator at
# independent sensor cadences, without rendering or access to the dev board.
add_executable(test_games_rest tests/test_games_rest.c
    ${COMPONENTS}/orb_games/orb_game_logic.c
    ${COMPONENTS}/orb_games/orb_game_motion.c ${COMPONENTS}/orb_games/orb_game_record.c)
target_include_directories(test_games_rest PRIVATE ${COMPONENTS}/orb_games/include)
target_compile_options(test_games_rest PRIVATE ${ORB_WARNINGS})
target_link_libraries(test_games_rest PRIVATE m)
add_test(NAME games_rest_traces COMMAND test_games_rest
    "${CMAKE_CURRENT_BINARY_DIR}/games3-beads.csv")

# Round-first Usage measures each physical line against the inner ring and
# exercises retained provider paging through production twist/pointer input.
add_executable(test_usage tests/test_usage.c sim_display.c sim_board.c)
target_include_directories(test_usage PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_usage PRIVATE orb m)
target_compile_options(test_usage PRIVATE ${ORB_WARNINGS})
foreach(mode default ax3 ax3-bold-rt)
    add_test(NAME usage_chords_${mode} COMMAND test_usage ${mode})
endforeach()

# Design 2.0: the public component kit is tested without migrating any app.
add_executable(kit_gallery kit_gallery.c emu_memory.c sim_display.c sim_board.c)
target_include_directories(kit_gallery PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kit_gallery PRIVATE orb m)
target_compile_options(kit_gallery PRIVATE ${ORB_WARNINGS})
add_executable(test_kit tests/test_kit.c emu_memory.c sim_display.c sim_board.c)
target_include_directories(test_kit PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_kit PRIVATE orb m)
target_compile_options(test_kit PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME kit_geometry COMMAND test_kit)
add_executable(kit_memory kit_memory.c emu_memory.c sim_display.c sim_board.c)
target_include_directories(kit_memory PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kit_memory PRIVATE orb m)
target_compile_options(kit_memory PRIVATE ${ORB_WARNINGS})
# Polish-text lane: truncation, AX rows, Bold Text, balanced/command wraps, readers.
add_executable(test_polish_text tests/test_polish_text.c emu_memory.c sim_display.c sim_board.c)
target_include_directories(test_polish_text PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_polish_text PRIVATE orb m)
target_compile_options(test_polish_text PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME polish_text COMMAND test_polish_text)
add_executable(test_lvgl_text tests/test_lvgl_text.c emu_memory.c sim_display.c sim_board.c)
target_include_directories(test_lvgl_text PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_lvgl_text PRIVATE orb m)
target_compile_options(test_lvgl_text PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME lvgl_text COMMAND test_lvgl_text)
add_executable(test_text_roles tests/test_text_roles.c emu_memory.c sim_display.c sim_board.c)
target_include_directories(test_text_roles PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_text_roles PRIVATE orb m)
target_compile_options(test_text_roles PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME text_roles COMMAND test_text_roles)
add_executable(test_polish_rows tests/test_polish_rows.c emu_memory.c sim_display.c sim_board.c)
target_include_directories(test_polish_rows PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_polish_rows PRIVATE orb m)
target_compile_options(test_polish_rows PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME polish_rows COMMAND test_polish_rows)
foreach(contract fit rim2 cache2)
    add_executable(test_kit_${contract} tests/test_kit_${contract}.c emu_memory.c sim_display.c sim_board.c)
    target_include_directories(test_kit_${contract} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test_kit_${contract} PRIVATE orb m)
    target_compile_options(test_kit_${contract} PRIVATE ${ORB_WARNINGS} -UNDEBUG)
    add_test(NAME kit_${contract} COMMAND test_kit_${contract})
endforeach()

# Same shell fixture records comparable retained payload before/after Faces 2.0.
add_executable(test_faces_baseline tests/test_faces_baseline.c emu_memory.c sim_display.c sim_board.c)
target_include_directories(test_faces_baseline PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_faces_baseline PRIVATE orb m)
target_compile_options(test_faces_baseline PRIVATE ${ORB_WARNINGS} -UNDEBUG)

add_executable(test_faces_layout tests/test_faces_layout.c emu_memory.c sim_display.c sim_board.c)
target_include_directories(test_faces_layout PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_faces_layout PRIVATE orb m)
target_compile_options(test_faces_layout PRIVATE ${ORB_WARNINGS} -UNDEBUG)
foreach(mode default ax3 ax3-bold-rt)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/faces-layout/${mode})
    add_test(NAME faces_layout_${mode} COMMAND test_faces_layout
        ${CMAKE_CURRENT_BINARY_DIR}/faces-layout/${mode} ${mode})
    set_tests_properties(faces_layout_${mode} PROPERTIES TIMEOUT 180)
endforeach()

# Design 2.0 system migration: real shell, isolated model/state matrix.
add_executable(test_m_system_layout tests/test_m_system_layout.c emu_memory.c sim_display.c sim_board.c)
target_include_directories(test_m_system_layout PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_m_system_layout PRIVATE orb m)
target_compile_options(test_m_system_layout PRIVATE ${ORB_WARNINGS} -UNDEBUG)
foreach(mode default ax3 ax3-bold-rt)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/m-system-layout/${mode})
    add_test(NAME m_system_layout_${mode} COMMAND test_m_system_layout
        ${CMAKE_CURRENT_BINARY_DIR}/m-system-layout/${mode} ${mode})
    set_tests_properties(m_system_layout_${mode} PROPERTIES TIMEOUT 180)
endforeach()
foreach(mode default ax3 ax3-bold-rt)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/kit-gallery/${mode})
    add_test(NAME kit_gallery_${mode} COMMAND kit_gallery
        ${CMAKE_CURRENT_BINARY_DIR}/kit-gallery/${mode} ${mode})
    set_tests_properties(kit_gallery_${mode} PROPERTIES TIMEOUT 120)
endforeach()

# Design 2.0 system-owned power and software-update sheets.
add_executable(test_system_sheets tests/test_system_sheets.c emu_memory.c sim_display.c sim_board.c)
target_include_directories(test_system_sheets PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_system_sheets PRIVATE orb m)
target_compile_options(test_system_sheets PRIVATE ${ORB_WARNINGS})
foreach(mode default ax3 ax3-bold-rt)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/system-sheets/${mode})
    add_test(NAME system_sheets_${mode} COMMAND test_system_sheets
        ${CMAKE_CURRENT_BINARY_DIR}/system-sheets/${mode} ${mode})
    set_tests_properties(system_sheets_${mode} PROPERTIES TIMEOUT 90)
endforeach()

# Owner-reported live-value regressions: actual board adapter, no hardware.
add_executable(test_power_brightness tests/test_power_brightness.c
    ${COMPONENTS}/orb_power/orb_power_board.c ${COMPONENTS}/orb_power/orb_power.c
    ${COMPONENTS}/orb_power/orb_power_imu.c)
target_include_directories(test_power_brightness PRIVATE
    ${COMPONENTS}/orb_power/include ${COMPONENTS}/orb_power/tests/host/stubs)
target_link_libraries(test_power_brightness PRIVATE m)
target_compile_options(test_power_brightness PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME power_brightness COMMAND test_power_brightness)

add_executable(test_live_values tests/test_live_values.c emu_memory.c sim_display.c sim_board.c)
target_include_directories(test_live_values PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_live_values PRIVATE orb m)
target_compile_options(test_live_values PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME live_values COMMAND test_live_values)
add_test(NAME kit_privacy COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_kit_privacy.py)

# Phase 0 parity: observe calls to the original production implementations.
# Only these executable-local object files rename symbols; liborb and firmware
# compilation/behaviour are unchanged. No replacement model or sound backend.
add_library(parity_observed OBJECT
    ${COMPONENTS}/orb_model/orb_model.c
    ${COMPONENTS}/orb_nav/orb_nav.c
    ${COMPONENTS}/orb_home_data/orb_home.c
    ${COMPONENTS}/orb_sound/orb_sound.c
    ${COMPONENTS}/orb_sound/orb_sound_preferences.c)
target_link_libraries(parity_observed PRIVATE orb)
target_compile_definitions(parity_observed PRIVATE
    orb_sound_play=parity_backend_sound_play
    orb_sound_preview=parity_backend_sound_preview
    orb_sound_stop=parity_backend_sound_stop
    orb_sound_preferences=parity_backend_sound_preferences
    orb_model_idle_open=parity_backend_idle_open
    orb_model_answer=parity_backend_answer
    orb_model_message=parity_backend_message
    orb_model_voice_request=parity_backend_voice_request
    orb_model_prefs_request=parity_backend_prefs_request
    orb_model_nowplaying_request=parity_backend_nowplaying_request
    orb_home_set=parity_backend_home_set
    orb_home_toggle=parity_backend_home_toggle
    orb_home_scene=parity_backend_home_scene
    orb_home_room_off=parity_backend_home_room_off
    orb_nav_rotary_bind=parity_backend_rotary_bind
    orb_nav_rotary_unbind=parity_backend_rotary_unbind
    orb_nav_rotary_set=parity_backend_rotary_set
    orb_nav_rotary_step=parity_backend_rotary_step
    orb_nav_tick=parity_backend_nav_tick
    orb_nav_touch=parity_backend_nav_touch
    orb_nav_init=parity_backend_nav_init
    orb_nav_reset=parity_backend_nav_reset
    ORB_SOUND_HOST_PACK="${CMAKE_CURRENT_SOURCE_DIR}/../tools/sounds/out/palette.orbs")
target_compile_options(parity_observed PRIVATE ${ORB_WARNINGS})
add_executable(orb_parity parity_main.c parity_shell.c parity_apps.c parity_observe.c alive_shots.c
    sim_display.c sim_board.c parity_storage.c
    ${COMPONENTS}/orb_time/orb_time.c ${COMPONENTS}/orb_time/orb_time_clock.c
    ${COMPONENTS}/orb_time/orb_time_record.c $<TARGET_OBJECTS:parity_observed>)
target_include_directories(orb_parity PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(orb_parity PRIVATE ORB_TIME_TEST_STORAGE=1 ORB_ALIVE_PARITY=1)
target_link_libraries(orb_parity PRIVATE orb_games_view m)
target_compile_options(orb_parity PRIVATE ${ORB_WARNINGS})
# Font retirement uses the exact parity presenters, but keeps each tree alive
# across every directed accessibility pair. No approval/send/unlock fixture is
# replayed by this test, and all fixture state remains process-local.
add_executable(test_font_surfaces ${COMPONENTS}/orb_fonts/tests/host/test_async_settings.c
    ${COMPONENTS}/orb_fonts/tests/host/test_font_surfaces.c
    parity_shell.c parity_apps.c parity_observe.c sim_display.c sim_board.c parity_storage.c
    ${COMPONENTS}/orb_time/orb_time.c ${COMPONENTS}/orb_time/orb_time_clock.c
    ${COMPONENTS}/orb_time/orb_time_record.c $<TARGET_OBJECTS:parity_observed>)
target_include_directories(test_font_surfaces PRIVATE
    ${COMPONENTS}/orb_fonts/tests/host/async_stubs ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(test_font_surfaces PRIVATE ORB_FONTS_SURFACE_TEST=1 ORB_TIME_TEST_STORAGE=1
    ORB_ASYNC_TEST_PACK_PATH="${COMPONENTS}/orb_fonts/pack/orb_fonts.pack")
target_link_libraries(test_font_surfaces PRIVATE orb_games_view m)
target_compile_options(test_font_surfaces PRIVATE ${ORB_WARNINGS})
set(FONT_LIFETIME_SCENARIOS
    navigation
    face-orb face-meridian face-activity face-always-on face-meridian-always-on face-activity-always-on
    apps.default agents.default sessions.default session.default usage.default idle.default dot.default
    approval.default question.default message.default stack.card.message notifications.default
    notifications.detail.retained control.default control.home control.volume voice.review spoken.default volume-hud.default
    settings.root settings.display settings.access settings.access.text-size settings.access.bold
    settings.glass settings.units settings.temperature settings.sources settings.sound settings.sound.volume
    settings.palette settings.quiet settings.battery settings.about settings.licences
    home-rooms home-room home-light home-scenes home-sensors home-thermostat
    timer-idle timer-running timer-custom-editor stopwatch-idle stopwatch-laps alarm-idle alarm-weekdays
    reminders-empty reminders-detail alarm-ringing
    play-picker marble-start marble-play marble-pause marble-result orbit-start orbit-play orbit-pause
    snake-start snake-play snake-pause pop-start pop-play pop-pause beads-play beads-clock
    weather.current onboarding.default system.setup system.pairing updates.mandatory.offered
    updates.mandatory.downloading updates.mandatory.verifying system.identify system.connection
    power-sheet.default offline.default charging.default compass.default time.alarm-layer)
foreach(scenario IN LISTS FONT_LIFETIME_SCENARIOS)
    add_test(NAME font_lifetime_${scenario} COMMAND test_font_surfaces --surface ${scenario})
    set_tests_properties(font_lifetime_${scenario} PROPERTIES TIMEOUT 300)
endforeach()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/parity-capture" CACHE PATH "Parity capture destination (empty/new)")
set(BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}/parity-base" CACHE PATH "Parity baseline directory")
set(NEW_DIR "${CMAKE_CURRENT_BINARY_DIR}/parity-new" CACHE PATH "Parity candidate directory")
add_custom_target(parity-capture
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/parity.py parity-capture
        "${OUT_DIR}" --binary $<TARGET_FILE:orb_parity> --jobs 4
    DEPENDS orb_parity USES_TERMINAL VERBATIM)
add_custom_target(parity-compare
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/parity.py parity-compare
        "${BASE_DIR}" "${NEW_DIR}"
    USES_TERMINAL VERBATIM)
add_test(NAME parity_tools COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_parity.py)
add_test(NAME parity_trace_text COMMAND orb_parity --test-trace-text)
add_test(NAME parity_smoke COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/parity_repro.py --binary $<TARGET_FILE:orb_parity>)
# Faces/Lock/system-state polish (polish-faces-results.md).
add_test(NAME polish_faces_renders COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_polish_faces.py --binary $<TARGET_FILE:orb_parity>)
# Polish wave 1: offline/read-only treatment, copy, rings, readers (polish1-results.md).
add_test(NAME polish1_renders COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_polish1.py --binary $<TARGET_FILE:orb_parity>)
set_tests_properties(polish1_renders PROPERTIES PROCESSORS 4 TIMEOUT 120)
set_tests_properties(parity_smoke PROPERTIES PROCESSORS 4 TIMEOUT 120)
add_test(NAME parity_catalogue COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/parity_catalogue.py --binary $<TARGET_FILE:orb_parity>
    --matrix ${CMAKE_CURRENT_SOURCE_DIR}/../docs/agent-briefs/wave2/parity-harness.md)

# Focused behavior regressions use the actual shell/Home trees and virtual time.
add_executable(test_home_fixes1 tests/test_home_fixes1.c sim_display.c sim_board.c)
target_include_directories(test_home_fixes1 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_home_fixes1 PRIVATE orb home_wire_fixture m)
target_compile_options(test_home_fixes1 PRIVATE ${ORB_WARNINGS})
foreach(mode default ax3-bold-rt)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/fixes1/${mode})
    foreach(case detail-context scene-context topology back-cue scene-cue shell-context shell-iris interrupted-iris iris-tokens non-accessory-tokens)
        add_test(NAME fixes1_home_${case}_${mode} COMMAND test_home_fixes1 ${case} ${mode})
        set_tests_properties(fixes1_home_${case}_${mode} PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)
    endforeach()
endforeach()

add_executable(test_fixes1_shell tests/test_fixes1_shell.c sim_display.c sim_board.c)
target_include_directories(test_fixes1_shell PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_fixes1_shell PRIVATE orb m)
target_compile_options(test_fixes1_shell PRIVATE ${ORB_WARNINGS})
foreach(case privacy alerts gallery close)
    add_test(NAME fixes1_shell_${case}_default COMMAND test_fixes1_shell ${case}
        ${CMAKE_CURRENT_BINARY_DIR}/fixes1/default)
    add_test(NAME fixes1_shell_${case}_ax3 COMMAND test_fixes1_shell ${case}
        ${CMAKE_CURRENT_BINARY_DIR}/fixes1/ax3-bold-rt ax3)
endforeach()

add_executable(test_session_fixes1 tests/test_session_fixes1.c sim_display.c sim_board.c)
target_include_directories(test_session_fixes1 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_session_fixes1 PRIVATE orb m)
target_compile_options(test_session_fixes1 PRIVATE ${ORB_WARNINGS})
foreach(mode default ax3 rm)
    set(SESSION_OUT ${CMAKE_CURRENT_BINARY_DIR}/fixes1/${mode})
    if(mode STREQUAL "ax3")
        set(SESSION_OUT ${CMAKE_CURRENT_BINARY_DIR}/fixes1/ax3-bold-rt)
    endif()
    file(MAKE_DIRECTORY ${SESSION_OUT})
    add_test(NAME fixes1_session_iris_${mode} COMMAND test_session_fixes1 ${mode} ${SESSION_OUT})
    file(MAKE_DIRECTORY ${SESSION_OUT}/idle)
    add_test(NAME fixes1_idle_session_iris_${mode} COMMAND test_session_fixes1 ${mode} ${SESSION_OUT}/idle idle)
endforeach()

# Focused behavior corrections preserve actual receipt/control/chrome contracts.
add_executable(test_workspace_fixes tests/test_workspace_fixes.c sim_display.c sim_board.c)
target_include_directories(test_workspace_fixes PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_workspace_fixes PRIVATE orb m)
target_compile_options(test_workspace_fixes PRIVATE ${ORB_WARNINGS})
foreach(mode default ax3-bold-rt)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/fixes1-workspace/${mode})
    foreach(behavior receipts disabled empty)
        add_test(NAME workspace_fixes_${behavior}_${mode} COMMAND test_workspace_fixes
            ${behavior} ${mode} ${CMAKE_CURRENT_BINARY_DIR}/fixes1-workspace/${mode})
    endforeach()
endforeach()

add_executable(test_system_qa tests/test_system_qa.c emu_memory.c sim_display.c sim_board.c)
target_include_directories(test_system_qa PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_system_qa PRIVATE orb m)
target_compile_options(test_system_qa PRIVATE ${ORB_WARNINGS} -UNDEBUG)
foreach(mode default ax3-bold-rt)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/qa-system/${mode})
    foreach(behavior context connectivity title stack-clock home-batch walk)
        add_test(NAME qa_system_${behavior}_${mode} COMMAND test_system_qa ${behavior} ${mode}
            ${CMAKE_CURRENT_BINARY_DIR}/qa-system/${mode})
    endforeach()
endforeach()

add_executable(test_system_face_context tests/test_system_face_context.c sim_display.c sim_board.c)
target_include_directories(test_system_face_context PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${COMPONENTS}/orb_shell)
target_link_libraries(test_system_face_context PRIVATE orb m)
target_compile_options(test_system_face_context PRIVATE ${ORB_WARNINGS} -UNDEBUG)
foreach(mode default ax3-bold-rt)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/qa-system-face/${mode})
    add_test(NAME qa_system_face_${mode} COMMAND test_system_face_context ${mode}
        ${CMAKE_CURRENT_BINARY_DIR}/qa-system-face/${mode})
endforeach()

add_executable(test_system_sound_save tests/test_system_sound_save.c sim_display.c sim_board.c)
target_include_directories(test_system_sound_save PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${COMPONENTS}/orb_shell)
target_link_libraries(test_system_sound_save PRIVATE orb m)
target_compile_options(test_system_sound_save PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME qa_system_sound_save COMMAND test_system_sound_save)

add_executable(test_system_home_context tests/test_system_home_context.c emu_memory.c sim_display.c sim_board.c)
target_include_directories(test_system_home_context PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_system_home_context PRIVATE orb m)
target_compile_options(test_system_home_context PRIVATE ${ORB_WARNINGS} -UNDEBUG)
foreach(mode default ax3-bold-rt)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/qa-system-home/${mode})
    add_test(NAME qa_system_home_${mode} COMMAND test_system_home_context ${mode}
        ${CMAKE_CURRENT_BINARY_DIR}/qa-system-home/${mode})
endforeach()

add_executable(test_qa_shell tests/test_qa_shell.c emu_memory.c sim_display.c sim_board.c)
target_include_directories(test_qa_shell PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_qa_shell PRIVATE orb m)
target_compile_options(test_qa_shell PRIVATE ${ORB_WARNINGS} -UNDEBUG)
foreach(mode default ax3-bold-rt)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/qa-shell/${mode})
    add_test(NAME qa_shell_${mode} COMMAND test_qa_shell ${CMAKE_CURRENT_BINARY_DIR}/qa-shell/${mode} ${mode})
endforeach()

include(${CMAKE_CURRENT_SOURCE_DIR}/screen-framework.cmake)
target_link_libraries(orb PUBLIC orb_screen)

# Real chord source lifetime and worker dispatch. The sanitizer targets include
# orb_ui.c directly, instrumenting its private painter and lease transitions.
foreach(chord_lifetime_target test_chord_lifetime test_chord_lifetime_asan)
    add_executable(${chord_lifetime_target} tests/test_chord_lifetime.c sim_display.c sim_board.c)
    target_include_directories(${chord_lifetime_target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${chord_lifetime_target} PRIVATE orb m)
    target_compile_options(${chord_lifetime_target} PRIVATE ${ORB_WARNINGS})
endforeach()
add_executable(test_chord_list_asan tests/test_chord_list_asan.c sim_display.c sim_board.c)
target_include_directories(test_chord_list_asan PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_chord_list_asan PRIVATE orb m)
target_compile_options(test_chord_list_asan PRIVATE ${ORB_WARNINGS})
foreach(chord_asan_target test_chord_lifetime_asan test_chord_list_asan)
    target_sources(${chord_asan_target} PRIVATE ${COMPONENTS}/orb_ui/orb_round_source.c
        ${COMPONENTS}/orb_ui/orb_round_text.c)
    target_compile_options(${chord_asan_target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${chord_asan_target} PRIVATE -fsanitize=address,undefined)
endforeach()
add_test(NAME chord_lifetime COMMAND test_chord_lifetime)
add_test(NAME chord_lifetime_asan COMMAND test_chord_lifetime_asan)
add_test(NAME chord_list_asan COMMAND test_chord_list_asan)
set_tests_properties(chord_lifetime chord_lifetime_asan chord_list_asan PROPERTIES TIMEOUT 30)
set_tests_properties(chord_lifetime_asan chord_list_asan PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0:halt_on_error=1;UBSAN_OPTIONS=halt_on_error=1")

# Phase 4c: real preference service with/without storage, and presenter lifetime.
add_executable(test_settings_memory
    ${COMPONENTS}/orb_settings/tests/host/test_settings_memory.c
    ${COMPONENTS}/orb_settings/orb_settings.c)
target_include_directories(test_settings_memory PRIVATE ${COMPONENTS}/orb_settings/include)
target_link_libraries(test_settings_memory PRIVATE pthread)
target_compile_options(test_settings_memory PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME settings_memory COMMAND test_settings_memory)

add_library(settings_test_fs OBJECT ${COMPONENTS}/orb_fs/orb_fs.c)
target_include_directories(settings_test_fs PRIVATE ${COMPONENTS}/orb_fs/include
    ${COMPONENTS}/orb_fs ${COMPONENTS}/orb_fs/tests/host)
target_compile_definitions(settings_test_fs PRIVATE rename=test_rename _POSIX_C_SOURCE=200809L)
target_compile_options(settings_test_fs PRIVATE ${ORB_WARNINGS})
add_executable(test_settings_storage
    ${COMPONENTS}/orb_settings/tests/host/test_settings_storage.c
    ${COMPONENTS}/orb_settings/orb_settings.c $<TARGET_OBJECTS:settings_test_fs>)
target_include_directories(test_settings_storage PRIVATE ${COMPONENTS}/orb_settings/include
    ${COMPONENTS}/orb_fs/include ${COMPONENTS}/orb_fs ${COMPONENTS}/orb_fs/tests/host)
target_compile_definitions(test_settings_storage PRIVATE ORB_SETTINGS_TEST_STORAGE=1 _POSIX_C_SOURCE=200809L)
target_link_libraries(test_settings_storage PRIVATE pthread)
target_compile_options(test_settings_storage PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME settings_storage COMMAND test_settings_storage)

add_executable(test_faces_settings tests/test_faces_settings.c
    ${COMPONENTS}/orb_settings/orb_settings.c sim_display.c sim_board.c)
target_include_directories(test_faces_settings PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(test_faces_settings PRIVATE ORB_SETTINGS_TEST_STORAGE=1
    orb_fs_ready=faces_settings_fs_ready orb_fs_app_prepare=faces_settings_fs_app_prepare)
target_link_libraries(test_faces_settings PRIVATE orb m)
target_compile_options(test_faces_settings PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME faces_settings COMMAND test_faces_settings)

# Cross-owner fixes2: actual workspace fixtures and live Time option sheets.
add_executable(test_fixes2_workspace tests/test_fixes2_workspace.c sim_display.c sim_board.c)
target_include_directories(test_fixes2_workspace PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_fixes2_workspace PRIVATE orb m)
target_compile_options(test_fixes2_workspace PRIVATE ${ORB_WARNINGS} -UNDEBUG)
foreach(mode default ax3-bold-rt)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/fixes2-evidence/${mode})
    foreach(behavior idle preview-display preview-notification alarm-sheet)
        add_test(NAME fixes2_${behavior}_${mode} COMMAND test_fixes2_workspace ${behavior} ${mode}
            ${CMAKE_CURRENT_BINARY_DIR}/fixes2-evidence/${mode})
    endforeach()
endforeach()

# Faces/transitions polish: iris-close token and the committed lock face (polish-faces-results.md).
add_executable(test_polish_faces_workspace tests/test_polish_faces_workspace.c sim_display.c sim_board.c)
target_include_directories(test_polish_faces_workspace PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_polish_faces_workspace PRIVATE orb m)
target_compile_options(test_polish_faces_workspace PRIVATE ${ORB_WARNINGS} -UNDEBUG)
foreach(behavior iris-close-token iris-frame-zero lock-chosen-face charging-live-updates arrival-title-lane editor-layers)
    add_test(NAME polish_faces_${behavior} COMMAND test_polish_faces_workspace ${behavior})
endforeach()

add_executable(test_kit_focus tests/test_kit_focus.c emu_memory.c sim_display.c sim_board.c)
target_include_directories(test_kit_focus PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_kit_focus PRIVATE orb m)
target_compile_options(test_kit_focus PRIVATE ${ORB_WARNINGS} -UNDEBUG)
foreach(mode default ax3-bold-rt)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/fixes2-kit/${mode})
    foreach(behavior selector glide)
        add_test(NAME fixes2_kit_${behavior}_${mode} COMMAND test_kit_focus ${behavior}
            ${CMAKE_CURRENT_BINARY_DIR}/fixes2-kit/${mode} ${mode})
    endforeach()
endforeach()

add_executable(test_fixes2_onboarding tests/test_fixes2_onboarding.c sim_display.c sim_board.c)
target_include_directories(test_fixes2_onboarding PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_fixes2_onboarding PRIVATE orb m)
target_compile_options(test_fixes2_onboarding PRIVATE ${ORB_WARNINGS} -UNDEBUG)
foreach(mode default ax3-bold-rt)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/fixes2-onboarding/${mode})
    add_test(NAME fixes2_onboarding_${mode} COMMAND test_fixes2_onboarding ${mode}
        ${CMAKE_CURRENT_BINARY_DIR}/fixes2-onboarding/${mode})
endforeach()

# Named round-stage receipt formatting is shared with the physical benchmark.
add_executable(test_round_receipt tests/test_round_receipt.c sim_display.c sim_board.c
    ${COMPONENTS}/orb_devtools/orb_devtools_round.c)
target_include_directories(test_round_receipt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${COMPONENTS}/orb_devtools
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/round_stubs)
target_link_libraries(test_round_receipt PRIVATE orb)
target_compile_options(test_round_receipt PRIVATE ${ORB_WARNINGS})
add_test(NAME round_receipt COMMAND test_round_receipt)

foreach(beads_case draw tasks)
    foreach(beads_mode normal asan)
        set(beads_target test_game_beads_${beads_case}_${beads_mode})
        add_executable(${beads_target}
            tests/test_game_beads_${beads_case}.c tests/beads_native_reference.c
            ${COMPONENTS}/orb_games/orb_game_native.c)
        target_include_directories(${beads_target} PRIVATE ${COMPONENTS}/orb_games)
        target_link_libraries(${beads_target} PRIVATE orb m)
        target_compile_options(${beads_target} PRIVATE ${ORB_WARNINGS} -UNDEBUG)
        if(beads_mode STREQUAL "asan")
            target_compile_options(${beads_target} PRIVATE
                -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
            target_link_options(${beads_target} PRIVATE -fsanitize=address,undefined)
        endif()
        add_test(NAME game_beads_${beads_case}_${beads_mode} COMMAND ${beads_target})
    endforeach()
endforeach()

# Frozen original prefix measurement checks bounded exact memoization and OOM.
foreach(round_cuts_target test_round_cuts test_round_cuts_asan)
    add_executable(${round_cuts_target} tests/test_round_cuts.c sim_display.c sim_board.c
        ${COMPONENTS}/orb_ui/orb_round_text.c)
    target_include_directories(${round_cuts_target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${COMPONENTS}/orb_ui)
    target_link_libraries(${round_cuts_target} PRIVATE orb m)
    target_compile_options(${round_cuts_target} PRIVATE ${ORB_WARNINGS})
endforeach()
target_compile_options(test_round_cuts_asan PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
target_link_options(test_round_cuts_asan PRIVATE -fsanitize=address,undefined)
add_test(NAME round_cuts COMMAND test_round_cuts)
add_test(NAME round_cuts_asan COMMAND test_round_cuts_asan)
set_tests_properties(round_cuts round_cuts_asan PROPERTIES TIMEOUT 60)
set_tests_properties(round_cuts_asan PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0:halt_on_error=1;UBSAN_OPTIONS=halt_on_error=1")

add_executable(test_game_orbit_lifetime tests/test_game_orbit_lifetime.c
    ${COMPONENTS}/orb_games/orb_game_native.c)
target_link_libraries(test_game_orbit_lifetime PRIVATE orb m)
target_compile_options(test_game_orbit_lifetime PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME game_orbit_lifetime COMMAND test_game_orbit_lifetime)
add_executable(test_game_orbit_lifetime_asan tests/test_game_orbit_lifetime.c
    ${COMPONENTS}/orb_games/orb_game_native.c)
target_link_libraries(test_game_orbit_lifetime_asan PRIVATE orb m)
target_compile_options(test_game_orbit_lifetime_asan PRIVATE ${ORB_WARNINGS}
    -UNDEBUG -fsanitize=address,undefined -fno-omit-frame-pointer)
target_link_options(test_game_orbit_lifetime_asan PRIVATE -fsanitize=address,undefined)
add_test(NAME game_orbit_lifetime_asan COMMAND test_game_orbit_lifetime_asan)

# Frozen source packing bytes, late palette overflow and scratch/cache OOM.
foreach(round_pack_target test_round_pack test_round_pack_asan)
    add_executable(${round_pack_target} tests/test_round_pack.c tests/round_pack_reference.c
        sim_display.c sim_board.c ${COMPONENTS}/orb_ui/orb_round_source.c)
    target_include_directories(${round_pack_target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${COMPONENTS}/orb_ui)
    target_link_libraries(${round_pack_target} PRIVATE orb m)
    target_compile_options(${round_pack_target} PRIVATE ${ORB_WARNINGS})
endforeach()
target_compile_options(test_round_pack_asan PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
target_link_options(test_round_pack_asan PRIVATE -fsanitize=address,undefined)
add_test(NAME round_pack COMMAND test_round_pack)
add_test(NAME round_pack_asan COMMAND test_round_pack_asan)
set_tests_properties(round_pack round_pack_asan PROPERTIES TIMEOUT 30)
set_tests_properties(round_pack_asan PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0:halt_on_error=1;UBSAN_OPTIONS=halt_on_error=1")

# Cold ring scratch preserves exact mask/cache bytes and all refusal cleanup.
foreach(ring_scratch_target test_game_ring_scratch test_game_ring_scratch_asan)
    add_executable(${ring_scratch_target} tests/test_game_ring_scratch.c sim_display.c sim_board.c)
    target_include_directories(${ring_scratch_target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${COMPONENTS}/orb_games)
    target_link_libraries(${ring_scratch_target} PRIVATE orb m)
    target_compile_options(${ring_scratch_target} PRIVATE ${ORB_WARNINGS})
endforeach()
target_compile_options(test_game_ring_scratch_asan PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
target_link_options(test_game_ring_scratch_asan PRIVATE -fsanitize=address,undefined)
add_test(NAME game_ring_scratch COMMAND test_game_ring_scratch)
add_test(NAME game_ring_scratch_asan COMMAND test_game_ring_scratch_asan)
set_tests_properties(game_ring_scratch game_ring_scratch_asan PROPERTIES TIMEOUT 30)
set_tests_properties(game_ring_scratch_asan PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0:halt_on_error=1;UBSAN_OPTIONS=halt_on_error=1")

add_test(NAME game_beads_bins COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_game_beads_bins.py)

add_executable(test_settings_about tests/test_settings_about.c tests/orb_screen_lifecycle.c sim_display.c sim_board.c)
target_include_directories(test_settings_about PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/tests ${COMPONENTS}/orb_shell)
target_link_libraries(test_settings_about PRIVATE orb orb_screen m)
target_compile_options(test_settings_about PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME settings_about COMMAND test_settings_about)

add_executable(test_app_home tests/test_app_home.c tests/orb_screen_lifecycle.c sim_display.c sim_board.c)
target_include_directories(test_app_home PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/tests
    ${COMPONENTS}/orb_shell ${COMPONENTS}/orb_app_home)
target_link_libraries(test_app_home PRIVATE orb orb_screen m)
target_compile_options(test_app_home PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME app_home COMMAND test_app_home)

add_executable(test_app_time tests/test_app_time.c tests/orb_screen_lifecycle.c sim_display.c sim_board.c)
target_include_directories(test_app_time PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/tests
    ${COMPONENTS}/orb_shell)
target_link_libraries(test_app_time PRIVATE orb orb_screen m)
target_compile_options(test_app_time PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME app_time COMMAND test_app_time)

add_executable(test_settings_units tests/test_settings_units.c tests/orb_screen_lifecycle.c sim_display.c sim_board.c)
target_include_directories(test_settings_units PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(test_settings_units PRIVATE orb orb_screen m)
target_compile_options(test_settings_units PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME settings_units COMMAND test_settings_units)

add_executable(test_settings_root tests/test_settings_root.c tests/orb_screen_lifecycle.c sim_display.c sim_board.c)
target_include_directories(test_settings_root PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(test_settings_root PRIVATE orb orb_screen m)
target_compile_options(test_settings_root PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME settings_root COMMAND test_settings_root)

add_executable(test_settings_stable_choices tests/test_settings_stable_choices.c sim_display.c sim_board.c)
target_include_directories(test_settings_stable_choices PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_settings_stable_choices PRIVATE orb orb_screen m)
target_compile_options(test_settings_stable_choices PRIVATE ${ORB_WARNINGS} -UNDEBUG)
foreach(choice glass temperature text-size text-size-reflow)
    add_test(NAME settings_stable_choice_${choice} COMMAND test_settings_stable_choices ${choice})
endforeach()

add_executable(test_settings_admission tests/test_settings_admission.c tests/orb_screen_lifecycle.c sim_display.c sim_board.c)
target_include_directories(test_settings_admission PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/tests ${COMPONENTS}/orb_shell)
target_link_libraries(test_settings_admission PRIVATE orb orb_screen m)
target_compile_options(test_settings_admission PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME settings_admission COMMAND test_settings_admission)

add_executable(test_workspace_settings_admission tests/test_workspace_settings_admission.c sim_display.c sim_board.c)
target_include_directories(test_workspace_settings_admission PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${COMPONENTS}/orb_shell)
target_link_libraries(test_workspace_settings_admission PRIVATE orb orb_screen m)
target_compile_options(test_workspace_settings_admission PRIVATE ${ORB_WARNINGS} -UNDEBUG)
foreach(admission_case fresh warm back partial)
    add_test(NAME workspace_settings_admission_${admission_case} COMMAND test_workspace_settings_admission ${admission_case})
endforeach()

add_executable(test_settings_sources tests/test_settings_sources.c tests/orb_screen_lifecycle.c sim_display.c sim_board.c)
target_include_directories(test_settings_sources PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(test_settings_sources PRIVATE orb orb_screen m)
target_compile_options(test_settings_sources PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME settings_sources COMMAND test_settings_sources)

add_executable(test_settings_accessibility tests/test_settings_accessibility.c tests/orb_screen_lifecycle.c sim_display.c sim_board.c)
target_include_directories(test_settings_accessibility PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(test_settings_accessibility PRIVATE orb orb_screen m)
target_compile_options(test_settings_accessibility PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME settings_accessibility COMMAND test_settings_accessibility)

add_executable(test_settings_battery tests/test_settings_battery.c tests/orb_screen_lifecycle.c sim_display.c sim_board.c)
target_include_directories(test_settings_battery PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(test_settings_battery PRIVATE orb orb_screen m)
target_compile_options(test_settings_battery PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME settings_battery COMMAND test_settings_battery)
add_executable(test_weather_app tests/test_weather_app.c tests/orb_screen_lifecycle.c sim_display.c sim_board.c)
target_include_directories(test_weather_app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/tests ${COMPONENTS}/orb_shell)
target_link_libraries(test_weather_app PRIVATE orb orb_screen m)
target_compile_options(test_weather_app PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME weather_app COMMAND test_weather_app)
add_executable(test_weather_route tests/test_weather_route.c sim_display.c sim_board.c)
target_include_directories(test_weather_route PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_weather_route PRIVATE orb m)
target_compile_options(test_weather_route PRIVATE ${ORB_WARNINGS})
add_test(NAME weather_route COMMAND test_weather_route)

add_executable(test_settings_display tests/test_settings_display.c tests/orb_screen_lifecycle.c sim_display.c sim_board.c)
target_include_directories(test_settings_display PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(test_settings_display PRIVATE orb orb_screen m)
target_compile_options(test_settings_display PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME settings_display COMMAND test_settings_display)

add_executable(test_settings_sound tests/test_settings_sound.c tests/orb_screen_lifecycle.c sim_display.c sim_board.c)
target_include_directories(test_settings_sound PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(test_settings_sound PRIVATE orb orb_screen m)
target_compile_options(test_settings_sound PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME settings_sound COMMAND test_settings_sound)

add_executable(test_settings_sound_host tests/test_settings_sound_host.c sim_display.c sim_board.c)
target_include_directories(test_settings_sound_host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${COMPONENTS}/orb_shell)
target_link_libraries(test_settings_sound_host PRIVATE orb orb_screen m)
target_compile_options(test_settings_sound_host PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME settings_sound_host COMMAND test_settings_sound_host)

# Separate processes retain independent orb_memory lifetime peak receipts.
add_executable(test_workspace_peak tests/test_workspace_peak.c sim_display.c sim_board.c)
target_include_directories(test_workspace_peak PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${COMPONENTS}/orb_shell
    ${COMPONENTS}/orb_devtools)
target_link_libraries(test_workspace_peak PRIVATE orb m)
target_compile_options(test_workspace_peak PRIVATE ${ORB_WARNINGS} -UNDEBUG)
foreach(workload home scroll-agents scroll-rooms scroll-settings scroll-notifications scroll-favourites)
    add_test(NAME workspace_peak_${workload} COMMAND test_workspace_peak ${workload})
endforeach()

add_executable(test_workspace_snapshots tests/test_workspace_snapshots.c sim_display.c sim_board.c)
target_include_directories(test_workspace_snapshots PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_workspace_snapshots PRIVATE orb m)
target_compile_options(test_workspace_snapshots PRIVATE ${ORB_WARNINGS} -UNDEBUG
    -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
target_link_options(test_workspace_snapshots PRIVATE -fsanitize=address,undefined)
foreach(snapshot_case sharing live-first preview-first external cow-model cow-home constructor)
    add_test(NAME workspace_snapshots_${snapshot_case} COMMAND test_workspace_snapshots ${snapshot_case})
    set_tests_properties(workspace_snapshots_${snapshot_case} PROPERTIES
        ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0;UBSAN_OPTIONS=halt_on_error=1")
endforeach()

add_executable(test_app_faces tests/test_app_faces.c tests/orb_screen_lifecycle.c sim_display.c sim_board.c)
target_include_directories(test_app_faces PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/tests
    ${COMPONENTS}/orb_shell)
target_link_libraries(test_app_faces PRIVATE orb orb_screen m)
target_compile_options(test_app_faces PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME app_faces COMMAND test_app_faces)

add_executable(test_app_faces_storage tests/test_app_faces.c tests/orb_screen_lifecycle.c
    ${COMPONENTS}/orb_settings/orb_settings.c sim_display.c sim_board.c)
target_include_directories(test_app_faces_storage PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/tests
    ${COMPONENTS}/orb_shell)
target_compile_definitions(test_app_faces_storage PRIVATE ORB_SETTINGS_TEST_STORAGE=1
    orb_fs_ready=app_faces_fs_ready orb_fs_app_prepare=app_faces_fs_app_prepare)
target_link_libraries(test_app_faces_storage PRIVATE orb orb_screen m)
target_compile_options(test_app_faces_storage PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME app_faces_storage COMMAND test_app_faces_storage)

add_executable(test_app_play tests/test_app_play.c tests/orb_screen_lifecycle.c sim_display.c sim_board.c)
target_include_directories(test_app_play PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/tests ${COMPONENTS}/orb_shell)
target_link_libraries(test_app_play PRIVATE orb_games_view orb_screen m)
target_compile_options(test_app_play PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME app_play COMMAND test_app_play)

add_executable(test_app_agents tests/test_app_agents.c tests/orb_screen_lifecycle.c sim_display.c sim_board.c)
target_include_directories(test_app_agents PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/tests ${COMPONENTS}/orb_shell)
target_link_libraries(test_app_agents PRIVATE orb orb_screen m)
target_compile_options(test_app_agents PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME app_agents COMMAND test_app_agents)

add_executable(test_notification_history tests/test_notification_history.c)
target_link_libraries(test_notification_history PRIVATE orb m)
target_compile_options(test_notification_history PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME notification_history COMMAND test_notification_history)

add_executable(test_notifications_view tests/test_notifications_view.c sim_display.c sim_board.c)
target_include_directories(test_notifications_view PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_notifications_view PRIVATE orb m)
target_compile_options(test_notifications_view PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME notifications_view COMMAND test_notifications_view)

add_executable(test_app_voice_spoken tests/test_app_voice_spoken.c tests/orb_screen_lifecycle.c sim_display.c sim_board.c)
target_include_directories(test_app_voice_spoken PRIVATE ${COMPONENTS}/orb_shell)
target_link_libraries(test_app_voice_spoken PRIVATE orb m)
target_compile_options(test_app_voice_spoken PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME app_voice_spoken COMMAND test_app_voice_spoken)

add_executable(test_app_voice tests/test_app_voice.c tests/orb_screen_lifecycle.c sim_display.c sim_board.c)
target_include_directories(test_app_voice PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/tests ${COMPONENTS}/orb_shell)
target_link_libraries(test_app_voice PRIVATE orb m)
target_compile_options(test_app_voice PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME app_voice COMMAND test_app_voice)

add_executable(test_system_charging tests/test_system_charging.c tests/orb_screen_lifecycle.c sim_display.c sim_board.c)
target_include_directories(test_system_charging PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/tests ${COMPONENTS}/orb_shell)
target_link_libraries(test_system_charging PRIVATE orb m)
target_compile_options(test_system_charging PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME system_charging COMMAND test_system_charging)

add_executable(test_control_screen tests/test_control_screen.c tests/orb_screen_lifecycle.c sim_display.c sim_board.c)
target_include_directories(test_control_screen PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/tests ${COMPONENTS}/orb_shell)
target_link_libraries(test_control_screen PRIVATE orb m)
target_compile_options(test_control_screen PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME control_screen COMMAND test_control_screen)

add_executable(test_system_descriptors tests/test_system_descriptors.c tests/orb_screen_lifecycle.c sim_display.c sim_board.c)
target_include_directories(test_system_descriptors PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/tests ${COMPONENTS}/orb_shell)
target_link_libraries(test_system_descriptors PRIVATE orb m)
target_compile_options(test_system_descriptors PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME system_descriptors COMMAND test_system_descriptors)

add_executable(test_apps_screen tests/test_apps_screen.c tests/orb_screen_lifecycle.c sim_display.c sim_board.c)
target_include_directories(test_apps_screen PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/tests ${COMPONENTS}/orb_shell)
target_link_libraries(test_apps_screen PRIVATE orb m)
target_compile_options(test_apps_screen PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME apps_screen COMMAND test_apps_screen)

add_executable(test_stack_screen tests/test_stack_screen.c tests/orb_screen_lifecycle.c sim_display.c sim_board.c)
target_include_directories(test_stack_screen PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/tests ${COMPONENTS}/orb_shell)
target_link_libraries(test_stack_screen PRIVATE orb m)
target_compile_options(test_stack_screen PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME stack_screen COMMAND test_stack_screen)

# Mac diagnostics and recovery use the production screen lifecycle.
add_executable(test_system_mac tests/test_system_mac.c tests/orb_screen_lifecycle.c sim_display.c sim_board.c)
target_include_directories(test_system_mac PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/tests ${COMPONENTS}/orb_shell)
target_link_libraries(test_system_mac PRIVATE orb m)
target_compile_options(test_system_mac PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME system_mac COMMAND test_system_mac)

add_executable(test_system_ota tests/test_system_ota.c tests/orb_screen_lifecycle.c sim_display.c sim_board.c)
target_include_directories(test_system_ota PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/tests ${COMPONENTS}/orb_shell)
target_link_libraries(test_system_ota PRIVATE orb m)
target_compile_options(test_system_ota PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME system_ota COMMAND test_system_ota)

add_executable(test_system_boot tests/test_system_boot.c tests/orb_screen_lifecycle.c sim_display.c sim_board.c)
target_include_directories(test_system_boot PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/tests ${COMPONENTS}/orb_shell)
target_link_libraries(test_system_boot PRIVATE orb m)
target_compile_options(test_system_boot PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME system_boot COMMAND test_system_boot)

add_executable(test_app_notifications tests/test_app_notifications.c tests/orb_screen_lifecycle.c sim_display.c sim_board.c)
target_include_directories(test_app_notifications PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/tests ${COMPONENTS}/orb_shell)
target_link_libraries(test_app_notifications PRIVATE orb m)
target_compile_options(test_app_notifications PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME app_notifications COMMAND test_app_notifications)

add_test(NAME radio_net_host COMMAND nice make -C "${COMPONENTS}/orb_net/tests/host" radio -j4)
add_test(NAME radio_link_host COMMAND nice make -C "${COMPONENTS}/orb_link/tests/host" -j4)
add_test(NAME power_controller_host COMMAND nice make -C "${COMPONENTS}/orb_power/tests/host" test -j4)
set_tests_properties(radio_net_host radio_link_host power_controller_host PROPERTIES PROCESSORS 4 RESOURCE_LOCK radio_host_build)

add_executable(test_imu_sleep_traces ${COMPONENTS}/orb_power/tests/host/test_imu_sleep_traces.c
    ${COMPONENTS}/orb_power/orb_power.c ${COMPONENTS}/orb_power/orb_power_imu.c)
target_include_directories(test_imu_sleep_traces PRIVATE ${FIRMWARE}/main
    ${COMPONENTS}/orb_power/include ${COMPONENTS}/orb_buttons/include)
target_compile_options(test_imu_sleep_traces PRIVATE ${ORB_WARNINGS} -UNDEBUG)
target_link_libraries(test_imu_sleep_traces PRIVATE m)
add_test(NAME imu_sleep_traces COMMAND test_imu_sleep_traces)
add_test(NAME display_ui_fairness COMMAND python3 ${COMPONENTS}/orb_display/tests/host/test_ui_fairness.py)
add_test(NAME display_touch_poll COMMAND python3 ${COMPONENTS}/orb_display/tests/host/test_touch_poll.py)

add_executable(test_light_sleep_policy ${COMPONENTS}/orb_power/tests/host/test_light_sleep_policy.c)
target_include_directories(test_light_sleep_policy PRIVATE ${COMPONENTS}/orb_power/include)
target_compile_options(test_light_sleep_policy PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME light_sleep_policy COMMAND test_light_sleep_policy)

add_test(NAME sleep_service COMMAND python3 ${FIRMWARE}/main/tests/host/test_sleep_service.py)
add_test(NAME sleep_waits COMMAND python3 ${FIRMWARE}/main/tests/host/test_sleep_waits.py)
add_test(NAME calm_power_safety COMMAND python3 ${FIRMWARE}/main/tests/host/test_calm_power_safety.py)
add_executable(test_model_host ${COMPONENTS}/orb_model/tests/host/test_model.c)
target_link_libraries(test_model_host PRIVATE orb pthread)
target_compile_options(test_model_host PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME model_host COMMAND test_model_host)

add_test(NAME light_sleep_runtime COMMAND python3 ${COMPONENTS}/orb_power/tests/host/test_light_sleep_runtime.py)
add_test(NAME power_services COMMAND python3 ${COMPONENTS}/orb_power/tests/host/test_power_services.py)
add_executable(test_devtools_wire ${COMPONENTS}/orb_devtools/tests/host/test_wire.c ${ORB_CJSON_DIR}/cJSON.c)
target_include_directories(test_devtools_wire PRIVATE ${COMPONENTS}/orb_devtools ${ORB_CJSON_DIR})
target_compile_options(test_devtools_wire PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME devtools_wire COMMAND test_devtools_wire)
add_test(NAME devtools_power COMMAND python3 ${COMPONENTS}/orb_devtools/tests/host/test_power.py)

add_test(NAME calm_imu_sleep COMMAND python3 ${FIRMWARE}/main/tests/host/test_calm_imu_sleep.py)
# Count actual production text work without adding telemetry to firmware. Only
# definition names change: calls, including calls within these files, go through
# the test's counter wrappers. The implementations are re-read on source change.
set(LABEL_WORK_SOURCES)
foreach(source font/lv_font.c misc/lv_text.c draw/sw/lv_draw_sw_letter.c)
    set(path ${LVGL_DIR}/src/${source})
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${path})
    file(READ ${path} instrumented)
    foreach(pair
        "bool lv_font_get_glyph_dsc_internal|bool label_work_glyph_impl"
        "const void * lv_font_get_glyph_bitmap_internal|const void * label_work_bitmap_impl"
        "void lv_text_get_size_attributes|void label_work_size_impl"
        "uint32_t lv_text_get_next_line|uint32_t label_work_line_impl"
        "int32_t lv_text_get_line_width|int32_t label_work_width_impl"
        "void lv_draw_sw_label|void label_work_draw_impl"
        "f->get_glyph_dsc(f,|label_work_font_lookup(f,"
        "placeholder_font->get_glyph_dsc(placeholder_font,|label_work_font_lookup(placeholder_font,")
        string(REPLACE "|" ";" names "${pair}")
        list(GET names 0 original)
        list(GET names 1 wrapped)
        string(REPLACE "${original}" "${wrapped}" instrumented "${instrumented}")
    endforeach()
    get_filename_component(name ${source} NAME)
    set(output ${CMAKE_CURRENT_BINARY_DIR}/label-work-${name})
    file(WRITE ${output} "#include \"label_work_probe.h\"\n${instrumented}")
    list(APPEND LABEL_WORK_SOURCES ${output})
endforeach()
add_executable(test_label_work tests/test_label_work.c ${LABEL_WORK_SOURCES} sim_display.c sim_board.c)
target_include_directories(test_label_work PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/tests
    ${LVGL_DIR}/src/font ${LVGL_DIR}/src/misc ${LVGL_DIR}/src/draw/sw ${COMPONENTS}/orb_shell
    ${COMPONENTS}/orb_devtools)
target_link_libraries(test_label_work PRIVATE orb_games_view m)
target_compile_options(test_label_work PRIVATE ${ORB_WARNINGS} -UNDEBUG)
foreach(scene orbit wake-orbit wake-iris list face iris)
    add_test(NAME label_work_${scene} COMMAND test_label_work ${scene})
    set_tests_properties(label_work_${scene} PROPERTIES TIMEOUT 30)
endforeach()

add_executable(test_label_ink_clip tests/test_label_ink_clip.c)
target_link_libraries(test_label_ink_clip PRIVATE lvgl m)
target_compile_options(test_label_ink_clip PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME label_ink_clip COMMAND test_label_ink_clip)
add_test(NAME devtools_bench_power COMMAND python3
    ${COMPONENTS}/orb_devtools/tests/host/test_bench_power.py)

foreach(alive_test motion controls effects home screens faces)
    add_executable(test_alive_${alive_test} tests/test_alive_${alive_test}.c emu_memory.c sim_display.c sim_board.c)
    target_include_directories(test_alive_${alive_test} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test_alive_${alive_test} PRIVATE orb home_wire_fixture m)
    target_compile_options(test_alive_${alive_test} PRIVATE ${ORB_WARNINGS} -UNDEBUG)
    add_test(NAME alive_${alive_test} COMMAND test_alive_${alive_test})
endforeach()

foreach(mode active dimmed always-on)
    add_test(NAME alive_os_${mode} COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_alive_os.py
        --binary $<TARGET_FILE:orb_parity> --mode ${mode})
    set_tests_properties(alive_os_${mode} PROPERTIES TIMEOUT 300)
endforeach()

add_executable(orb_alive_shots alive_shots.c emu_memory.c sim_display.c sim_board.c)
target_include_directories(orb_alive_shots PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(orb_alive_shots PRIVATE orb home_wire_fixture m)
target_compile_options(orb_alive_shots PRIVATE ${ORB_WARNINGS})

add_executable(test_letter_task_bounds tests/test_letter_task_bounds.c)
target_link_libraries(test_letter_task_bounds PRIVATE lvgl m)
target_compile_options(test_letter_task_bounds PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME letter_task_bounds COMMAND test_letter_task_bounds)

add_executable(test_event_owner_lifetime tests/test_event_owner_lifetime.c sim_display.c sim_board.c)
target_include_directories(test_event_owner_lifetime PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_event_owner_lifetime PRIVATE orb m)
target_compile_options(test_event_owner_lifetime PRIVATE ${ORB_WARNINGS} -UNDEBUG)
add_test(NAME event_owner_lifetime COMMAND test_event_owner_lifetime)
