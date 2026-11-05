# gugu host simulator and unit tests — plain CMake (no ESP-IDF build system), see PLAN.md 2026-11-02.
# The portable app/ sources compile against tiny shims; board/audio/net/store have *_host.c backends;
# SDL2 provides the 466×466 round window, mouse-as-touch, keyboard-as-keys and audio.
cmake_minimum_required(VERSION 3.16)
project(gugu_host C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()
set(GUGU_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
set(LVGL_DIR ${GUGU_ROOT}/managed_components/lvgl__lvgl)
if(NOT EXISTS ${LVGL_DIR}/CMakeLists.txt)
    message(FATAL_ERROR "LVGL not found at ${LVGL_DIR}: run `idf.py reconfigure` at the repo root once so the component manager downloads it")
endif()
if(DEFINED ENV{IDF_PATH})
    set(IDF_PATH $ENV{IDF_PATH})
else()
    set(IDF_PATH $ENV{HOME}/esp/esp-idf)
endif()

# LVGL from the managed component, with our lv_conf.h
set(LV_CONF_PATH ${CMAKE_CURRENT_LIST_DIR}/lv_conf.h CACHE STRING "" FORCE)
set(LV_CONF_BUILD_DISABLE_EXAMPLES ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_DEMOS ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_THORVG_INTERNAL ON CACHE BOOL "" FORCE)
add_subdirectory(${LVGL_DIR} ${CMAKE_BINARY_DIR}/lvgl EXCLUDE_FROM_ALL)

find_package(SDL2 REQUIRED CONFIG)
find_package(CURL REQUIRED)
if(NOT DEFINED OPENSSL_ROOT_DIR AND EXISTS /opt/homebrew/opt/openssl@3)
    set(OPENSSL_ROOT_DIR /opt/homebrew/opt/openssl@3)
endif()
find_package(OpenSSL REQUIRED)

# cJSON: the same sources the device uses (IDF's json component)
add_library(cjson STATIC ${IDF_PATH}/components/json/cJSON/cJSON.c)
target_include_directories(cjson PUBLIC ${IDF_PATH}/components/json/cJSON)

file(GLOB_RECURSE APP_SRCS ${GUGU_ROOT}/app/*.c)
list(FILTER APP_SRCS EXCLUDE REGEX ".*_esp\\.c$")

add_executable(gugu_host main.c ${APP_SRCS})
target_compile_definitions(gugu_host PRIVATE GUGU_HOST=1 LV_CONF_INCLUDE_SIMPLE=1)
target_include_directories(gugu_host PRIVATE ${GUGU_ROOT}/app ${CMAKE_CURRENT_LIST_DIR}/shim ${CMAKE_CURRENT_LIST_DIR} ${LVGL_DIR})
target_link_libraries(gugu_host PRIVATE lvgl cjson SDL2::SDL2 CURL::libcurl OpenSSL::SSL OpenSSL::Crypto pthread m)
target_compile_definitions(gugu_host PRIVATE GUGU_REPO_ROOT="${GUGU_ROOT}")
target_compile_options(gugu_host PRIVATE -Wall -Wno-unused-function)

# ---- unit tests: the portable modules only (no board/ui/SDL) ----
file(GLOB TEST_SRCS ${GUGU_ROOT}/test/*.c)
set(CORE_SRCS ${APP_SRCS})
list(FILTER CORE_SRCS EXCLUDE REGEX ".*/app/app\\.c$")
list(FILTER CORE_SRCS EXCLUDE REGEX ".*/app/(board|ui|call|console|instinct)/.*")
list(FILTER CORE_SRCS EXCLUDE REGEX ".*/app/parent/parent_host\\.c$")   # the page's API is tested, its web server is not
list(FILTER CORE_SRCS EXCLUDE REGEX ".*/app/audio/audio_host\\.c$")
add_executable(gugu_tests ${TEST_SRCS} ${CORE_SRCS})
target_compile_definitions(gugu_tests PRIVATE GUGU_HOST=1)
target_include_directories(gugu_tests PRIVATE ${GUGU_ROOT}/app ${GUGU_ROOT}/test ${CMAKE_CURRENT_LIST_DIR}/shim)   # tests may reach private headers by path
target_link_libraries(gugu_tests PRIVATE lvgl cjson CURL::libcurl OpenSSL::SSL OpenSSL::Crypto pthread m)   # lvgl: the art module uses its TJpgDec
target_compile_definitions(gugu_tests PRIVATE GUGU_REPO_ROOT="${GUGU_ROOT}" LV_CONF_INCLUDE_SIMPLE=1)
target_include_directories(gugu_tests PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${LVGL_DIR})
enable_testing()
add_test(NAME unit COMMAND gugu_tests)
