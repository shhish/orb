cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS "../..")
# Only main and what it pulls in, not every Orb component
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(orb_net_test)
