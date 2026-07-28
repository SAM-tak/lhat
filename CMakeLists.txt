cmake_minimum_required(VERSION 3.25)

project(lhat
    VERSION 0.1.0
    DESCRIPTION "L^ - a bytecode interpreted glue programming language"
    LANGUAGES C
)

# ---- Language level -------------------------------------------------------
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)  # -std=c11, not -std=gnu11

# ---- Warnings -------------------------------------------------------------
if(MSVC)
    # /utf-8 : treat sources as UTF-8 (otherwise MSVC assumes the system
    #          codepage and mangles non-ASCII comments and literals).
    add_compile_options(/W4 /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ---- Targets --------------------------------------------------------------
add_executable(lhat
    src/main.c
)

target_compile_definitions(lhat PRIVATE LHAT_VERSION="${PROJECT_VERSION}")

install(TARGETS lhat RUNTIME DESTINATION bin)
