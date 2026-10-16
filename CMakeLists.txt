# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Wi-Fi credentials live in sdkconfig.secrets, which is git-ignored (see sdkconfig.secrets.example).
# Setting SDKCONFIG_DEFAULTS replaces the implicit sdkconfig.defaults, so list every existing file explicitly.
set(defaults_files "")
foreach(name sdkconfig.defaults sdkconfig.secrets)
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/${name}")
        list(APPEND defaults_files "${name}")
    endif()
endforeach()
# Test build that ACKs frames on the car's bus (CONFIG_CAN_ACK_FRAMES), kept apart from the car image:
#   idf.py -B build-ack -D SDKCONFIG=build-ack/sdkconfig -D CAN_ACK_BUILD=1 build
if(CAN_ACK_BUILD)
    list(APPEND defaults_files "sdkconfig.ack")
endif()
set(SDKCONFIG_DEFAULTS "${defaults_files}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(can-sniffer)
