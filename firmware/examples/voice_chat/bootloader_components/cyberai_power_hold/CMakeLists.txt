# Power-hold hook for the CyberAI EchoEar clone.
# Built only when `idf.py select-board --board cyberai_echoear` is active; for every other
# board this registers an empty component so the bootloader is unchanged.
set(board_name_file "${CMAKE_CURRENT_LIST_DIR}/../../components/gen_bmgr_codes/agent_board_name.txt")
set(board_name "")
if(EXISTS "${board_name_file}")
    file(READ "${board_name_file}" board_name)
    string(STRIP "${board_name}" board_name)
endif()

if(board_name STREQUAL "cyberai_echoear")
    idf_component_register(SRCS "hooks.c")
else()
    idf_component_register()
endif()
