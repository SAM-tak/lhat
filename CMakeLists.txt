cmake_minimum_required(VERSION 3.25)

project(lhat
    VERSION 0.1.0
    DESCRIPTION "L^ - a bytecode interpreted glue programming language"
    LANGUAGES C
)

option(LHAT_BUILD_TESTS "Build the L^ test suite" ON)
option(LHAT_BUILD_CLI "Build the lhat command line driver" ON)

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

# ---- The language ---------------------------------------------------------
# Everything but the driver, so a host embeds this and nothing else. It calls
# out to lhatport for memory and for reading a unit -- see src/port.h.
add_library(lhat
    src/source.c
    src/error.c
    src/token.c
    src/lexer.c
    src/ast.c
    src/parser.c
    src/type.c
    src/check.c
    src/program.c
    src/code.c
    src/vm.c
    src/value.c
    src/object.c
)

# A host gets include/ and nothing else. src/ holds names like source.h and
# value.h, which are too ordinary to put on someone else's include path --
# include/lhat.h reaches them and is the only header a host names.
target_include_directories(lhat
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_definitions(lhat PUBLIC LHAT_VERSION="${PROJECT_VERSION}")

# ---- What the language asks of its surroundings ---------------------------
# 05 の 8.9: the seam is the linker. A host with its own allocator, or its own
# way of reading a unit, copies these files, changes them, and leaves this
# target out of its link -- there is nothing to register and nothing to
# install before the first allocation.
add_library(lhatport
    port/alloc.c
    port/loader.c
)

target_include_directories(lhatport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# So that a replacement of these files, written against include/lhat.h alone,
# compiles the same way ours does.
target_include_directories(lhatport PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# The default is what anyone linking the language gets unless they say
# otherwise. PUBLIC so it carries through to whatever links lhat.
target_link_libraries(lhat PUBLIC lhatport)

# ---- Driver ---------------------------------------------------------------
if(LHAT_BUILD_CLI)
    add_executable(lhat_cli cli/main.c)
    set_target_properties(lhat_cli PROPERTIES OUTPUT_NAME lhat)
    target_link_libraries(lhat_cli PRIVATE lhat)
    install(TARGETS lhat_cli RUNTIME DESTINATION bin)
endif()

install(TARGETS lhat lhatport
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

# lhat.h reaches the rest by a path relative to itself, so the two directories
# are installed keeping the shape they have here. A host still names only
# <prefix>/include/lhat.h and puts only <prefix>/include on its path.
install(FILES include/lhat.h DESTINATION include)
install(DIRECTORY src/ DESTINATION src FILES_MATCHING PATTERN "*.h")

# ---- Tests ----------------------------------------------------------------
if(LHAT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
