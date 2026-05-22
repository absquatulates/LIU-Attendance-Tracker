# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/main/*.c)
idf_component_register(SRCS ${app_sources}
                        INCLUDE_DIRS "."
                        REQUIRES driver esp_timer spiffs esp_driver_spi esp_driver_gpio)