# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

# for IDF v4
set(PROJECT_VER "2.4.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
include_directories(${CMAKE_BINARY_DIR})

# Dynamically set PROJECT_VER based on ESP-IDF version
if(DEFINED IDF_VERSION_MAJOR AND DEFINED IDF_VERSION_MINOR AND DEFINED IDF_VERSION_PATCH)
    if(IDF_VERSION_MAJOR EQUAL 4 AND IDF_VERSION_MINOR EQUAL 4)
        set(PROJECT_VER "2.4.4")
    elseif(IDF_VERSION_MAJOR EQUAL 5 AND IDF_VERSION_MINOR EQUAL 2)
        set(PROJECT_VER "2.5.2")
    elseif(IDF_VERSION_MAJOR EQUAL 5 AND IDF_VERSION_MINOR EQUAL 0)
        set(PROJECT_VER "2.5.0")
    else()
        set(PROJECT_VER "2.x.x")
    endif()
endif()

# Read sdkconfig for display and BT options
set(SDKCONFIG_FILE "${CMAKE_SOURCE_DIR}/sdkconfig")
if(EXISTS "${SDKCONFIG_FILE}")
    file(READ "${SDKCONFIG_FILE}" SDKCONFIG_CONTENT)
else()
    set(SDKCONFIG_CONTENT "")
endif()

# Helper macro to check if a config is set
macro(get_config_option option_name out_var)
    string(REGEX MATCH "CONFIG_${option_name}=[y\"]" _match "${SDKCONFIG_CONTENT}")
    if(_match)
        set(${out_var} 1)
    else()
        set(${out_var} 0)
    endif()
endmacro()

# Determine display suffix
set(DISPLAY_SUFFIX "")
get_config_option("DISPLAY_TYPE_MONO" DISPLAY_MONO)
get_config_option("DISPLAY_TYPE_COLOR" DISPLAY_COLOR)
get_config_option("DISPLAY_TYPE_NONE" DISPLAY_NONE)
get_config_option("DISPLAY_TYPE_ALL" DISPLAY_ALL)
if(DISPLAY_MONO)
    set(DISPLAY_SUFFIX "_M")
elseif(DISPLAY_COLOR)
    set(DISPLAY_SUFFIX "_C")
elseif(DISPLAY_NONE)
    set(DISPLAY_SUFFIX "_N")
elseif(DISPLAY_ALL)
    set(DISPLAY_SUFFIX "_A")
endif()

# Determine BT suffix
set(BT_SUFFIX "")
get_config_option("BT_SPEAKER_MODE" BT_SPEAKER_MODE)
if(BT_SPEAKER_MODE)
    set(BT_SUFFIX "_BT")
endif()

# Set base project name by target
if(IDF_TARGET STREQUAL "esp32")
    set(PROJECT_NAME "KaRadio32")
elseif(IDF_TARGET STREQUAL "esp32s3")
    set(PROJECT_NAME "KaRadio32s3")
else()
    set(PROJECT_NAME "KaRadio32x")
endif()

# Compose final project name
set(END_PROJECT_NAME "${PROJECT_NAME}${DISPLAY_SUFFIX}${BT_SUFFIX}")

# Set project name and version
project(${END_PROJECT_NAME} VERSION ${PROJECT_VER})

configure_file(
    ${CMAKE_SOURCE_DIR}/main/include/KaRadio_version.h.in
    ${CMAKE_BINARY_DIR}/KaRadio_version.h
    @ONLY
)

# include($ENV{IDF_PATH}/tools/cmake/project.cmake)

idf_build_set_property(COMPILE_OPTIONS
    "-Wno-unused-const-variable"
    "-fmacro-prefix-map=${CMAKE_SOURCE_DIR}=."
    "-fmacro-prefix-map=${IDF_PATH}=IDF"
    APPEND)


# --- Custom bootloader.bin rename/copy ---
set(BOOTLOADER_BIN "${CMAKE_BINARY_DIR}/bootloader/bootloader.bin")
set(BOOTLOADER_SUFFIX "${IDF_TARGET}")
set(BOOTLOADER_TARGET_BIN "${CMAKE_BINARY_DIR}/bootloader/bootloader${BOOTLOADER_SUFFIX}.bin")

add_custom_command(
    TARGET bootloader
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${BOOTLOADER_BIN}"
        "${BOOTLOADER_TARGET_BIN}"
    COMMENT "Copying bootloader.bin to bootloader${BOOTLOADER_SUFFIX}.bin"
)
