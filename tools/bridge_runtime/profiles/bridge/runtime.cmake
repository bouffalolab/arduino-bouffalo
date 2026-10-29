set(UNOR4_BL616CL_BOARD_OVERLAY_DIR
    ${CMAKE_CURRENT_LIST_DIR}/board_overlay)

sdk_add_include_directories(
    ${CMAKE_CURRENT_LIST_DIR}
    ${UNOR4_BL616CL_BOARD_OVERLAY_DIR}
)
target_sources(app PRIVATE
    ${UNOR4_BL616CL_BOARD_OVERLAY_DIR}/board_overlay.c
    ${UNOR4_BL616CL_BOARD_OVERLAY_DIR}/board_gpio_overlay.c
)
sdk_set_main_file(${CMAKE_CURRENT_LIST_DIR}/main.c)

project(arduino_bl616cl_bridge_runtime)
