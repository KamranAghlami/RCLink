file(GLOB_RECURSE SOURCES "src/*.c" "src/*.cpp")

idf_component_register(SRCS ${SOURCES} PRIV_INCLUDE_DIRS "src" REQUIRES board lua)

fatfs_create_spiflash_image(storage app PRESERVE_TIME)