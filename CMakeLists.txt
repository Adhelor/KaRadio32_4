# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)
set(PROJECT_VER "2.4.0")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(KaRadio32_4)

idf_build_set_property(COMPILE_OPTIONS "-Wno-unused-const-variable" APPEND)