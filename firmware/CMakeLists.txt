cmake_minimum_required(VERSION 3.22)

include("${CMAKE_CURRENT_LIST_DIR}/project-version.cmake")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(orb)

# A defaults overlay cannot override saved sdkconfig choices. Fail closed if a
# reused or manually selected config would put developer input into a release.
if(ORB_BUILD_CHANNEL STREQUAL "stable" AND CONFIG_ORB_DEVTOOLS)
    message(FATAL_ERROR
        "Stable builds require CONFIG_ORB_DEVTOOLS=n. Remove the stable build's "
        "sdkconfig and configure again with scripts/idf.sh --channel stable.")
endif()
