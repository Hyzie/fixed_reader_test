cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(PARTITION_TABLE_CSV_PATH ${CMAKE_CURRENT_LIST_DIR}/partitions.csv)

project(esp32-w5500-uart-control)
