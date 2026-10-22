cmake_minimum_required(VERSION 3.25)
project(lhat_rust LANGUAGES C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(LHAT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LHAT_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LHAT_BUILD_LSP OFF CACHE BOOL "" FORCE)
set(LHAT_BUILD_DAP OFF CACHE BOOL "" FORCE)
set(LHAT_BUILD_STDLIB OFF CACHE BOOL "" FORCE)
add_subdirectory(../../.. lhat)
add_library(lhat_rust_bridge STATIC bridge.c)
target_link_libraries(lhat_rust_bridge PRIVATE lhat)
# One predictable directory for both single- and multi-configuration builds.
foreach(target lhat_rust_bridge lhat lhatport)
    set_target_properties(${target} PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
        ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/lib")
endforeach()
