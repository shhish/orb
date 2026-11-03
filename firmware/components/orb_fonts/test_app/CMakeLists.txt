# Builds orb_fonts on its own with the firmware's flash layout: proves it compiles
# against Orb's LVGL fork and measures it (idf.py size-components). The app also
# checks every font at run time if flashed. See ../README.md.
cmake_minimum_required(VERSION 3.22)

# orb_fonts, and orb_lv_simd so LVGL blends with the S3 SIMD routines as the firmware does.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/.." "${CMAKE_CURRENT_LIST_DIR}/../../orb_lv_simd")
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(orb_fonts_test)
