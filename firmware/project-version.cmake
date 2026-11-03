# A previous `-D PROJECT_VER=...` must never become the next build's identity.
# scripts/idf.sh carries an explicit override in the environment for this process
# only; CMake cannot distinguish an old cache entry from a fresh -D argument.
unset(PROJECT_VER CACHE)
execute_process(COMMAND git -c maintenance.auto=false rev-parse --short=7 HEAD
                WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/.."
                OUTPUT_VARIABLE ORB_REV OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
if(NOT ORB_REV)
    set(ORB_REV "unknown")
endif()
set(PROJECT_VER "0.3.0-${ORB_REV}")
if(NOT "$ENV{ORB_PROJECT_VER}" STREQUAL "")
    set(PROJECT_VER "$ENV{ORB_PROJECT_VER}")
endif()

# idf.py checks requested cache entries before deciding whether to run CMake.
# The wrapper supplies the current intent every time so a changed HEAD or the
# end of a one-build override reconfigures, without slowing unchanged builds.
set(ORB_BUILD_VERSION "${PROJECT_VER}" CACHE INTERNAL "Last effective Orb version" FORCE)
