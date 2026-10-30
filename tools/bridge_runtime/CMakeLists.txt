cmake_minimum_required(VERSION 3.15)

find_package(bouffalo_sdk REQUIRED HINTS $ENV{BL_SDK_BASE})

if(CONFIG_ARDUINO_RUNTIME_PROFILE STREQUAL "bridge")
    include(${CMAKE_CURRENT_SOURCE_DIR}/profiles/bridge/runtime.cmake)
else()
    sdk_add_include_directories(.)
    sdk_set_main_file(main.c)
    project(arduino_bl616cl_runtime)
endif()
