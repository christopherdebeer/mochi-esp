# Top-level CMake for the eink-pet firmware project.
#
# This is the standard ESP-IDF project skeleton. The `project.cmake`
# include pulls in all the IDF build machinery; `project(eink-pet)`
# names the resulting binary.
#
# Run from this directory after sourcing `$IDF_PATH/export.sh`:
#   idf.py set-target esp32s3
#   idf.py build

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(eink-pet)
