idf_component_register()

# Build the partition for animation and icon image files
set(ASSETS_NAME "asset_test")
# The board manager generates CONFIG_ESP_BOARD_<NAME>, not CONFIG_BOARD_<NAME>,
# so also match on the selected board name.
if(CONFIG_BOARD_ESP_VOCAT_BOARD_V1_2
   OR CONFIG_ESP_BOARD_NAME STREQUAL "esp_vocat_board_v1_2"
   OR CONFIG_ESP_BOARD_NAME STREQUAL "cyberai_echoear")
    set(RESOLUTION "360_360")
elseif(CONFIG_BOARD_M5STACK_CORES3 OR CONFIG_BOARD_ESP_BOX_3)
    set(RESOLUTION "320_240")
else()
    message(STATUS "Unknown board, using default")
    set(RESOLUTION "320_240")
endif()
message(STATUS "Resolution: ${RESOLUTION}")

set(ASSETS_FILE "${CMAKE_BINARY_DIR}/${ASSETS_NAME}.bin")
# A board can override emote layout files in boards/<board>/emote_assets/<resolution>/;
# anything not found there falls back to the esp_emote_assets component.
set(BOARD_EMOTE_ASSETS "${CMAKE_CURRENT_LIST_DIR}/../boards/${CONFIG_ESP_BOARD_NAME}/emote_assets")
if(EXISTS "${BOARD_EMOTE_ASSETS}/${RESOLUTION}")
    message(STATUS "Using board emote asset overrides: ${BOARD_EMOTE_ASSETS}")
    build_speaker_assets_bin("anim_icon" ${RESOLUTION} ${ASSETS_FILE} ${CONFIG_MMAP_FILE_NAME_LENGTH} ${BOARD_EMOTE_ASSETS})
else()
    build_speaker_assets_bin("anim_icon" ${RESOLUTION} ${ASSETS_FILE} ${CONFIG_MMAP_FILE_NAME_LENGTH})
endif()
message(STATUS "Generated emote assets: ${ASSETS_FILE} -> anim_icon partition")
esptool_py_flash_to_partition(flash "anim_icon" "${ASSETS_FILE}")
