file(GLOB_RECURSE SOURCES "src/*.c" "src/*.cpp")

idf_component_register(SRCS ${SOURCES} PRIV_INCLUDE_DIRS "src" PRIV_REQUIRES board esp_http_server esp_rom lua)

if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
    littlefs_create_partition_image(storage app)
else()
    fail_at_build_time(littlefs "Windows does not support LittleFS partition generation")
endif()
