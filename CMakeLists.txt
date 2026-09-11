cmake_minimum_required(VERSION 3.5)

# Default version (fallback to minimum allowed IDF)
set(PROJECT_VER "2.3.5")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# --- Detect IDF version and adjust PROJECT_VER ---
if(DEFINED IDF_VERSION_MAJOR AND DEFINED IDF_VERSION_MINOR)
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

# --- Project (BEFORE using CONFIG_*) ---
project(KaRadio32 VERSION ${PROJECT_VER})

include_directories(${CMAKE_BINARY_DIR})

# =========================================================
#   ===          CONFIG-BASED SUFFIXES             =====
# =========================================================

# Display suffix
set(DISPLAY_SUFFIX "")
if(CONFIG_DISPLAY_TYPE_MONO)
    set(DISPLAY_SUFFIX "_M")
elseif(CONFIG_DISPLAY_TYPE_COLOR)
    set(DISPLAY_SUFFIX "_C")
elseif(CONFIG_DISPLAY_TYPE_NONE)
    set(DISPLAY_SUFFIX "_N")
elseif(CONFIG_DISPLAY_TYPE_ALL)
    set(DISPLAY_SUFFIX "_A")
endif()

# BT suffix
set(BT_SUFFIX "")
if(CONFIG_BT_SPEAKER_MODE)
    set(BT_SUFFIX "_BT")
endif()

# Target-based base name
if(IDF_TARGET STREQUAL "esp32")
    set(BASE_NAME "KaRadio32")
elseif(IDF_TARGET STREQUAL "esp32s3")
    set(BASE_NAME "KaRadio32s3")
else()
    set(BASE_NAME "KaRadio32x")
endif()

# Final firmware name
set(FW_NAME "${BASE_NAME}${DISPLAY_SUFFIX}${BT_SUFFIX}")

# Apply name to output binaries
idf_build_set_property(EXECUTABLE_NAME ${FW_NAME})

# =========================================================
#  === REVISION HANDLING (ON TARGET CHANGE) ==============
# =========================================================

set(REVISION_FILE "${CMAKE_SOURCE_DIR}/.revision")
set(TARGET_FILE "${CMAKE_SOURCE_DIR}/.last_target")

# init revision
if(NOT EXISTS ${REVISION_FILE})
    file(WRITE ${REVISION_FILE} "0")
endif()

file(READ ${REVISION_FILE} REVISION_NUMBER)
string(STRIP ${REVISION_NUMBER} REVISION_NUMBER)

# last target
if(EXISTS ${TARGET_FILE})
    file(READ ${TARGET_FILE} LAST_TARGET)
    string(STRIP ${LAST_TARGET} LAST_TARGET)
else()
    set(LAST_TARGET "")
endif()

# increment if target changed
if(NOT "${LAST_TARGET}" STREQUAL "${IDF_TARGET}")
    math(EXPR REVISION_NUMBER "${REVISION_NUMBER} + 1")
    file(WRITE ${REVISION_FILE} "${REVISION_NUMBER}")
    file(WRITE ${TARGET_FILE} "${IDF_TARGET}")
endif()

# =========================================================
# === VERSION HEADER ======================================
# =========================================================

configure_file(
    ${CMAKE_SOURCE_DIR}/main/include/KaRadio_version.h.in
    ${CMAKE_SOURCE_DIR}/main/include/KaRadio_version.h
    @ONLY
)
# =========================================================
# === BUILD OPTIONS =======================================
# =========================================================

idf_build_set_property(COMPILE_OPTIONS
    "-Wno-unused-const-variable"
    "-fmacro-prefix-map=${CMAKE_SOURCE_DIR}=."
    "-fmacro-prefix-map=${IDF_PATH}=IDF"
    APPEND)

# =========================================================
# === BOOTLOADER COPY =====================================
# =========================================================

set(BOOTLOADER_BIN "${CMAKE_BINARY_DIR}/bootloader/bootloader.bin")
set(BOOTLOADER_TARGET_BIN "${CMAKE_BINARY_DIR}/bootloader/bootloader_${IDF_TARGET}.bin")

add_custom_command(
    TARGET bootloader
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${BOOTLOADER_BIN}"
        "${BOOTLOADER_TARGET_BIN}"
    COMMENT "Copying bootloader.bin to bootloader_${IDF_TARGET}.bin"
)

# =========================================================
# === DEBUG INFO ==========================================
# =========================================================

message(STATUS "Firmware: ${FW_NAME}")
message(STATUS "Revision: ${REVISION_NUMBER}")
message(STATUS "Target: ${IDF_TARGET}")