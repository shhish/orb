# Builds orb_ota on its own, signed the way the real firmware is. orb_ota comes
# from firmware/components; components/orb_link here is a stub that takes
# precedence over the real one (project components win over EXTRA_COMPONENT_DIRS).
cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../..")
set(COMPONENTS main)
# Siblings orb_ota doesn't use, with their own dependencies (the BSP, LVGL, mDNS...).
set(EXCLUDE_COMPONENTS orb_board orb_display orb_ui orb_lv_simd orb_net)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(orb_ota_test)
