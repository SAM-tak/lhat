cmake_minimum_required(VERSION 3.25)

project(lhat
    VERSION 0.1.0
    DESCRIPTION "L^ - a bytecode interpreted glue programming language"
    LANGUAGES C
)

option(LHAT_BUILD_TESTS "Build the L^ test suite" ON)

# ---- Language level -------------------------------------------------------
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)  # -std=c11, not -std=gnu11

# ---- Warnings -------------------------------------------------------------
if(MSVC)
    # /utf-8 : treat sources as UTF-8 (otherwise MSVC assumes the system
    #          codepage and mangles non-ASCII comments and literals).
    add_compile_options(/W4 /utf-8)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ---- Core library ---------------------------------------------------------
# The language is meant to be embeddable, so everything except the command
# line driver lives in a library that the tests can link against directly.
add_library(lhat_core STATIC
    src/source.c
    src/token.c
    src/lexer.c
    src/ast.c
    src/parser.c
    src/type.c
    src/check.c
    src/value.c
)

target_include_directories(lhat_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(lhat_core PUBLIC LHAT_VERSION="${PROJECT_VERSION}")

# ---- Driver ---------------------------------------------------------------
add_executable(lhat src/main.c)
target_link_libraries(lhat PRIVATE lhat_core)

install(TARGETS lhat RUNTIME DESTINATION bin)

# ---- Tests ----------------------------------------------------------------
if(LHAT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
