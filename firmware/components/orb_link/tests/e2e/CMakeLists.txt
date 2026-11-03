# orb_link end to end on the host: ESP-IDF's linux target runs the real
# orb_link.c against hub_stub.py over TLS on 127.0.0.1.
cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../.."
                         "${CMAKE_CURRENT_LIST_DIR}/../../../orb_model"
                         "${CMAKE_CURRENT_LIST_DIR}/../../../orb_mem")
set(COMPONENTS main)

# A fixed version, for hub_stub.py to check in pair.begin and hello.
set(PROJECT_VER "0.2.1")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(orb_link_e2e)

# The client stores portMAX_DELAY in an int; with the linux port's 64-bit
# ticks clang flags it. Harmless here (it means "wait forever" either way).
idf_component_get_property(ws_lib espressif__esp_websocket_client COMPONENT_LIB)
target_compile_options(${ws_lib} PRIVATE -Wno-error=constant-conversion)
