cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../.."
                         "${CMAKE_CURRENT_LIST_DIR}/../../orb_model"
                         "${CMAKE_CURRENT_LIST_DIR}/../../orb_mem")
# Only main and what it needs, not every component under firmware/components.
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(orb_link_test)
