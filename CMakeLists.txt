cmake_minimum_required(VERSION 3.21)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT EXISTS ${CMAKE_SOURCE_DIR}/components/board)
  set(TARGET_BOARD UNSPECIFIED CACHE STRING "Target board to build for")
  set_property(CACHE TARGET_BOARD PROPERTY STRINGS T-Display-S3 T-Encoder-Pro)

  if(${TARGET_BOARD} STREQUAL T-Display-S3)
    set(TARGET_BOARD_URL "https://github.com/KamranAghlami/idf_component_t_display_s3")
  elseif(${TARGET_BOARD} STREQUAL T-Encoder-Pro)
    set(TARGET_BOARD_URL "https://github.com/KamranAghlami/idf_component_t_encoder_pro")
  else()
    message(FATAL_ERROR "TARGET_BOARD is unspecified.")
  endif()

  execute_process(
    COMMAND git clone ${TARGET_BOARD_URL} ${CMAKE_SOURCE_DIR}/components/board
    RESULT_VARIABLE clone_result
  )

  if(clone_result)
      message(FATAL_ERROR "Failed to clone the target board repository!")
  endif()
endif()

set(SDKCONFIG_DEFAULTS components/board/sdkconfig.defaults)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(RCLink LANGUAGES C CXX)
