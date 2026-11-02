# gugu firmware — ESP-IDF project.
# The application component is `app/` (ESP-IDF's "main" component, renamed per the
# "Renaming main component" procedure): it is added through EXTRA_COMPONENT_DIRS and lists
# its own IDF dependencies explicitly in app/CMakeLists.txt.
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/app")
set(COMPONENTS app)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(gugu)
