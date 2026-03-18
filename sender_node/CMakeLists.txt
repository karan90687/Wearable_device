cmake_minimum_required(VERSION 3.16)

# Include shared components from common/
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/../common/espnow_comm"
    "${CMAKE_CURRENT_SOURCE_DIR}/../common/protocol"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sender_node)
