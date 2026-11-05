cmake_minimum_required(VERSION 3.20)
project(e5_infoscreen_ui C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)

set(LVGL_DIR "" CACHE PATH "Extracted LVGL source tree")
set(CJSON_DIR "" CACHE PATH "Extracted cJSON source tree")
set(GEN_DIR "" CACHE PATH "Directory of generated headers")

find_package(PkgConfig REQUIRED)
pkg_check_modules(FREETYPE REQUIRED freetype2)
pkg_check_modules(DRM REQUIRED libdrm)
pkg_check_modules(JPEG REQUIRED libjpeg)

file(GLOB_RECURSE LVGL_SOURCES CONFIGURE_DEPENDS ${LVGL_DIR}/src/*.c)
add_library(lvgl STATIC ${LVGL_SOURCES})
target_include_directories(lvgl PUBLIC ${LVGL_DIR} ${CMAKE_SOURCE_DIR})
target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE)
target_compile_options(lvgl PRIVATE -O2 -w)

add_library(cjson STATIC ${CJSON_DIR}/cJSON.c)
target_include_directories(cjson PUBLIC ${CJSON_DIR})

file(GLOB APP_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.c ${CMAKE_SOURCE_DIR}/src/pages/*.c)
add_executable(e5-infoscreen-ui ${APP_SOURCES})
target_include_directories(e5-infoscreen-ui PRIVATE ${CMAKE_SOURCE_DIR}/src ${GEN_DIR} ${FREETYPE_INCLUDE_DIRS} ${DRM_INCLUDE_DIRS} ${JPEG_INCLUDE_DIRS})
target_compile_options(e5-infoscreen-ui PRIVATE -O2 -Wall -Wextra -Werror -Wno-unused-parameter)
target_link_options(e5-infoscreen-ui PRIVATE -static -s -Wl,--gc-sections)
target_link_libraries(e5-infoscreen-ui PRIVATE lvgl cjson ${FREETYPE_STATIC_LIBRARIES} ${JPEG_STATIC_LIBRARIES} m pthread)
