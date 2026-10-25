# SSH Runner ESP32-S3 firmware. Build only inside the pinned container:
#   tools/idf.ps1 build        (see tools/check.ps1)
cmake_minimum_required(VERSION 3.16)
# The release version (CHANGELOG.md), in the app descriptor and the boot log.
set(PROJECT_VER "0.2.0")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(flipper_eth_ssh)

# A changed Kconfig or sdkconfig.defaults must regenerate the build's config (sdkconfig.h):
# ESP-IDF does not re-run CMake for them, so without this an incremental build keeps the old
# config and a new option is silently compiled out.
file(GLOB _fes_config_inputs CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_LIST_DIR}/main/Kconfig*
    ${CMAKE_CURRENT_LIST_DIR}/components/*/Kconfig*
    ${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults*
    ${CMAKE_CURRENT_LIST_DIR}/sdkconfig.efusetest)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_fes_config_inputs})
