file(GLOB_RECURSE SOURCES "src/*.c" "src/*.cpp")

idf_component_register(SRCS ${SOURCES} PRIV_INCLUDE_DIRS "src" PRIV_REQUIRES application esp_http_server esp_partition app_update esp_rom lua)

find_program(PYTHON_COMMAND python3 REQUIRED)

execute_process(COMMAND ${PYTHON_COMMAND} ${CMAKE_SOURCE_DIR}/scripts/get_webui.py)

if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
  littlefs_create_partition_image(storage app)
else()
  fail_at_build_time(littlefs "Windows does not support LittleFS partition generation")
endif()
