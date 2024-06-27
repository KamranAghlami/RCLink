cmake_minimum_required(VERSION 3.21)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SDKCONFIG_DEFAULTS components/board/sdkconfig.defaults)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(RCLink LANGUAGES C CXX)
