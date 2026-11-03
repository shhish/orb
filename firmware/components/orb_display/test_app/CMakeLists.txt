# Builds orb_display (and what it needs) on its own: scripts that change it
# must keep this compiling. Flashed, it brings the panel up blank and logs stats.
cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../..")
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(orb_display_test_app)
