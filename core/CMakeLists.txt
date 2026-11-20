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

# sim.js, the game rules, embedded as a C string (the view ships the same file).
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/sim.js" SIM_JS)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/gen/sim_js.h" "#pragma once\nstatic const char kSimJs[] = R\"SIMJS(${SIM_JS})SIMJS\";\n")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/sim.js")

logos_module(
    NAME ${MODULE_NAME}
    SOURCES
        src/bubble_core_impl.h
        src/bubble_core_impl.cpp
        src/check.h
        src/check.cpp
        src/pow.h
        src/pow.cpp
    INCLUDE_DIRS src
)
target_include_directories(${MODULE_NAME}_module_plugin PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/gen")

find_path(SODIUM_INCLUDE_DIR sodium.h REQUIRED)
find_library(SODIUM_LIBRARY NAMES sodium libsodium REQUIRED)
target_include_directories(${MODULE_NAME}_module_plugin PRIVATE ${SODIUM_INCLUDE_DIR})
target_link_libraries(${MODULE_NAME}_module_plugin PRIVATE ${SODIUM_LIBRARY})

# QJSEngine runs sim.js to check a finished game.
find_package(Qt6 REQUIRED COMPONENTS Qml)
target_link_libraries(${MODULE_NAME}_module_plugin PRIVATE Qt6::Qml)
