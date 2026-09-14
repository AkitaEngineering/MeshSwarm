cmake_minimum_required(VERSION 3.16)

# ESP-IDF v5.x project. Distro CMake 4.x / arm-none-eabi-gcc will not configure this tree.
if(NOT DEFINED ENV{IDF_PATH} OR NOT EXISTS "$ENV{IDF_PATH}/tools/cmake/project.cmake")
    message(FATAL_ERROR
        "ESP-IDF is not loaded (IDF_PATH unset or invalid). "
        "Source ESP-IDF v5.x export.sh / export.fish, then run `idf.py set-target esp32` and `idf.py build`. "
        "Do not configure with system CMake 4.x or arm-none-eabi-gcc.")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(meshswarm)
