cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(SDKCONFIG_DEFAULTS ${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults)
include(${CMAKE_CURRENT_LIST_DIR}/../common/boards/add_board_sdkconfig_defaults.cmake)

# Only the voice-free parts of common/: app_common (agent, audio, RainMaker) stays out of this build
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../common/app_ui"
                         "${CMAKE_CURRENT_LIST_DIR}/../common/boards"
                         "${CMAKE_CURRENT_LIST_DIR}/../common/assets"
                         "${CMAKE_CURRENT_LIST_DIR}/../common/espressif2022__esp_emote_gfx")

project(desk_keys)
