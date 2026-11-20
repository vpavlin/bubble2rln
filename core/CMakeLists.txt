cmake_minimum_required(VERSION 3.14)
project(BubbleCoreModule LANGUAGES CXX)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/LogosModule.cmake")
    include(cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found. Set LOGOS_MODULE_BUILDER_ROOT or ensure logos-module-builder is in your flake inputs.")
endif()

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/metadata.json" METADATA_JSON)
string(JSON MODULE_NAME GET ${METADATA_JSON} name)

logos_module(
    NAME ${MODULE_NAME}
    SOURCES
        src/bubble_core_impl.h
        src/bubble_core_impl.cpp
        src/sim.h
        src/sim.cpp
        src/check.h
        src/check.cpp
        src/pow.h
        src/pow.cpp
    INCLUDE_DIRS src
)

find_path(SODIUM_INCLUDE_DIR sodium.h REQUIRED)
find_library(SODIUM_LIBRARY NAMES sodium libsodium REQUIRED)
target_include_directories(${MODULE_NAME}_module_plugin PRIVATE ${SODIUM_INCLUDE_DIR})
target_link_libraries(${MODULE_NAME}_module_plugin PRIVATE ${SODIUM_LIBRARY})
