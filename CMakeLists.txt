cmake_minimum_required(VERSION 3.25)

project(lhat
    VERSION 0.2.0
    DESCRIPTION "L^ - a bytecode interpreted glue programming language"
    LANGUAGES C
)

option(LHAT_BUILD_TESTS "Build the L^ test suite" ON)
option(LHAT_BUILD_CLI "Build the lhat command line driver" ON)
option(LHAT_BUILD_LSP "Build the L^ language server" ON)

# comments are kept and attached to the tree, for the language
# server to show and the visual editor to edit. Nothing in the language reads
# them, so a host that embeds the language and no tooling turns this off and
# pays for neither the table nor the walk that attaches it.
option(LHAT_WITH_COMMENTS "Keep comments and attach them to the syntax tree" ON)

# 07 の 4 章: the same bargain for what the checker learned about each name.
# Compiling and running read none of it -- it is recorded for the language
# server, which would otherwise resolve 8 章's scoping a second time and
# disagree in exactly the hard cases. A host embedding the language and no
# tooling turns this off and pays for neither the array nor the recording.
option(LHAT_WITH_RESOLUTIONS "Record what each name resolved to, for tooling" ON)

# Address (and, off MSVC, undefined-behaviour) sanitizers for the whole
# build. A switch rather than a default: the suite runs it before the risky
# refactors, not on every developer build.
option(LHAT_SANITIZE "Build with AddressSanitizer (+UBSan where available)" OFF)

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

# ---- Sanitizers ------------------------------------------------------------
if(LHAT_SANITIZE)
    if(MSVC)
        # MSVC's ASan refuses /RTC1, which the default Debug flags carry.
        string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
        add_compile_options(/fsanitize=address)
    else()
        add_compile_options(-fsanitize=address,undefined
                            -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

# ---- The language ---------------------------------------------------------
# Everything but the driver, so a host embeds this and nothing else. It calls
# out to lhatport for memory and for reading a unit -- see src/port.h.
add_library(lhat
    src/source.c
    src/error.c
    src/token.c
    src/number.c
    src/lexer.c
    src/ast.c
    src/parser.c
    src/type.c
    src/check.c
    src/check_expr.c
    src/check_stmt.c
    src/semantic.c
    src/program.c
    src/code.c
    src/compile.c
    src/rttype.c
    src/vm.c
    src/gc.c
    src/value.c
    src/object.c
)

# The version and the build knobs live in a generated header rather than in
# compile definitions: LHAT_WITH_COMMENTS changes the shape of LhatLexer and
# LhatNode, and a definition an installed consumer forgets to repeat would
# hand them a differently-sized struct without a compile error to show for
# it. Baked into a header, every translation unit that can see the structs
# agrees by construction.
if(LHAT_WITH_COMMENTS)
    set(LHAT_WITH_COMMENTS_VALUE 1)
else()
    set(LHAT_WITH_COMMENTS_VALUE 0)
endif()
if(LHAT_WITH_RESOLUTIONS)
    set(LHAT_WITH_RESOLUTIONS_VALUE 1)
else()
    set(LHAT_WITH_RESOLUTIONS_VALUE 0)
endif()
configure_file(include/lhat/version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/include/lhat/version.h)

# A host gets include/ and nothing else. src/ holds names like parser.h and
# type.h, which are too ordinary to put on someone else's include path --
# include/lhat.h reaches what a host runs with, and lhat/lexer.h is the one
# other header it may name (07 の 4 章: colouring an editor).
target_include_directories(lhat
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
           $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# 03 の 5.12: a write barrier that is missing does not fail where it is
# missing. The object is freed while still live and the program reads it back
# some time later, somewhere else -- so the collector checks its own invariant
# outright wherever that can be paid for. A pass over the heap per cycle, in
# builds that are already not fast.
target_compile_definitions(lhat PRIVATE $<$<CONFIG:Debug>:LHAT_GC_PARANOID>)

# ---- What the language asks of its surroundings ---------------------------
# 05 の 8.9: the seam is the linker. A host with its own allocator, or its own
# way of reading a unit, copies these files, changes them, and leaves this
# target out of its link -- there is nothing to register and nothing to
# install before the first allocation.
add_library(lhatport
    port/alloc.c
    port/loader.c
)

target_include_directories(lhatport
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:include>)

# So that a replacement of these files, written against include/lhat.h alone,
# compiles the same way ours does.
target_include_directories(lhatport PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# The default is what anyone linking the language gets unless they say
# otherwise. PUBLIC so it carries through to whatever links lhat.
target_link_libraries(lhat PUBLIC lhatport)

# ---- Threads --------------------------------------------------------------
# As port/thread.h says at its head, this is not something the core asks
# for -- the language runs on one thread. Only what stands beside the core
# needs it (stdlib/thread.c and lsp/), so it is a target of its own rather
# than part of lhatport, and `lhat` does not link it. C11's <threads.h> is
# not used: macOS does not have it, and MSVC only gained it in VS2022 17.8.
# Both pthreads and the Win32 synchronisation API predate that by years.
find_package(Threads REQUIRED)

add_library(lhatthread port/thread.c)
target_link_libraries(lhatthread PUBLIC Threads::Threads)

# Included as "port/thread.h" -- putting the bare header name on a path
# would collide with stdlib/thread.h. The Driver section below does the same
# for stdlib/*.h.
target_include_directories(lhatthread
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
           $<INSTALL_INTERFACE:include>)

# ---- Sample standard library ------------------------------------------------
# stdlib/ is kept at arm's length differently from port/: port is the
# default implementation the core links unless told otherwise, while stdlib
# is an optional library a host chooses to add. lhat_lsp does not link it --
# that it can not only be deleted whole but was never built in to begin with
# is what "sample" is meant to say. lhat_cli, being the sample driver, links
# it in the Driver section below for as long as LHAT_BUILD_STDLIB is ON.
option(LHAT_BUILD_STDLIB "Build the L^ sample standard library (stdlib/)" ON)

if(LHAT_BUILD_STDLIB)
    add_library(lhatstdlib
        stdlib/async.c
        stdlib/debug.c
        stdlib/error.c
        stdlib/io.c
        stdlib/math.c
        stdlib/random.c
        stdlib/regex.c
        stdlib/regex_engine.c
        stdlib/thread.c
    )
    # lhat.h is the whole of what it needs (no private path into src/) --
    # for the same reason port/ has none, this is itself the worked example
    # of "a library written against lhat.h alone". std.thread is the one
    # exception: an OS thread is outside lhat.h, so port/thread.h is added
    # for it. It appears in no stdlib/*.h, hence PRIVATE.
    target_include_directories(lhatstdlib
        PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
               $<INSTALL_INTERFACE:include>)
    target_link_libraries(lhatstdlib PUBLIC lhat PRIVATE lhatthread)

    install(TARGETS lhatstdlib EXPORT lhatTargets
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin)
    install(FILES stdlib/async.h stdlib/debug.h stdlib/error.h stdlib/io.h
            stdlib/random.h stdlib/regex.h
            stdlib/thread.h
        DESTINATION include/lhatstdlib)
endif()

# ---- Driver ---------------------------------------------------------------
if(LHAT_BUILD_CLI)
    add_executable(lhat_cli cli/main.c)
    set_target_properties(lhat_cli PROPERTIES OUTPUT_NAME lhat)
    target_link_libraries(lhat_cli PRIVATE lhat)
    # The dump and REPL paths read tokens, trees and each stage's results
    # directly -- the cli is an in-tree front end, so it names the internal
    # headers honestly instead of getting them by leakage.
    target_include_directories(lhat_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    # Being the sample driver, it registers the whole of stdlib/ when
    # stdlib/ was built (see bind_host_names in cli/main.c). With
    # LHAT_BUILD_STDLIB=OFF, cli/main.c still builds without the macro --
    # stdlib is optional, always.
    if(LHAT_BUILD_STDLIB)
        target_link_libraries(lhat_cli PRIVATE lhatstdlib)
        # The stdlib/*.h include each other flat, as "error.h", on the
        # understanding that they sit in one directory (see stdlib/io.c).
        # cli/main.c is the one that looks at them from outside, so the
        # repository root goes on its path and it writes "stdlib/foo.h".
        target_include_directories(lhat_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_definitions(lhat_cli PRIVATE LHAT_CLI_WITH_STDLIB)
    endif()
    install(TARGETS lhat_cli RUNTIME DESTINATION bin)
endif()

# ---- Language server --------------------------------------------------------
# A second, independent consumer of lhat.lib alongside the CLI -- it speaks
# LSP over stdio instead of a prompt. vendor/cjson is JSON-RPC's message
# shape; nothing about lhat's own build needs it, so it is scoped to here.
if(LHAT_BUILD_LSP)
    # 07 の 4 章: hover, semantic tokens and lhat/signature all answer from
    # what the checker recorded about each name, so the server is exactly the
    # consumer LHAT_WITH_RESOLUTIONS exists for. Turning that off and asking
    # for a server anyway is a contradiction worth naming here rather than
    # letting it surface as an undeclared identifier deep in lsp/.
    if(NOT LHAT_WITH_RESOLUTIONS)
        message(FATAL_ERROR
            "LHAT_BUILD_LSP needs LHAT_WITH_RESOLUTIONS: the language server "
            "answers from what the checker recorded about each name. Turn one "
            "of the two off.")
    endif()

    add_library(cjson STATIC vendor/cjson/cJSON.c)
    target_include_directories(cjson PUBLIC vendor/cjson)
    if(MSVC)
        target_compile_options(cjson PRIVATE /W0)
    else()
        target_compile_options(cjson PRIVATE -w)
    endif()

    add_executable(lhat_lsp
        lsp/main.c
        lsp/server.c
        lsp/worker.c
        lsp/transport.c
        lsp/rpc.c
        lsp/dispatch.c
        lsp/dispatch_table.c
        lsp/uri.c
        lsp/util.c
        lsp/document_store.c
        lsp/workspace.c
        lsp/host_config.c
        lsp/diagnostics.c
        lsp/position.c
        lsp/semantic_tokens.c
        lsp/ast_json.c
        lsp/hover.c
        lsp/queue.c
        lsp/handlers/initialize.c
        lsp/handlers/text_document_sync.c
        lsp/handlers/semantic_tokens.c
        lsp/handlers/ast.c
        lsp/handlers/hover.c
        lsp/handlers/signature.c
        lsp/signature.c
        lsp/handlers/definition.c
        lsp/definition.c
        lsp/resolution.c
    )
    # The binary is a language server, not "LSP" itself, so the executable
    # is named lhatls -- but the target, lsp/ and the archive/pdb byproducts
    # stay lhat-lsp/lhat_lsp; only RUNTIME_OUTPUT_NAME (the .exe) changes.
    set_target_properties(lhat_lsp PROPERTIES
        OUTPUT_NAME lhat-lsp
        RUNTIME_OUTPUT_NAME lhatls
    )
    target_link_libraries(lhat_lsp PRIVATE lhat cjson lhatthread)
    target_include_directories(lhat_lsp PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/lsp
    )
    install(TARGETS lhat_lsp RUNTIME DESTINATION bin)
endif()

# ---- Godot ------------------------------------------------------------------
# A third consumer of the library, beside the CLI and the server, and the only
# one that is neither an executable nor written in C: godot/ builds a
# GDExtension, which the engine loads at run time. It is off by default because
# it enables C++ and fetches godot-cpp, and an ordinary build of the language
# should do neither. The godot-* presets turn it on, alone in a tree of its own.
option(LHAT_BUILD_GDEXTENSION "Build the Godot GDExtension (godot/)" OFF)

if(LHAT_BUILD_GDEXTENSION)
    add_subdirectory(godot)
endif()

install(TARGETS lhat lhatport lhatthread EXPORT lhatTargets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

# lhatstdlib ships still naming lhat_thread_*, so the library and the header
# that resolve those go with it. Installed as a directory, so that whoever
# includes it still writes "port/thread.h".
install(FILES port/thread.h DESTINATION include/port)

# What ships is what a host may name: lhat.h and the headers under lhat/,
# in the shape they already have here. Nothing in src/ is installed -- the
# tree, the checker, the instruction set and the collector are the library's
# own, and install_smoke is what proves lhat.h no longer reaches any of them
# (it builds a host against this tree alone).
#
# lhat/lexer.h and lhat/token.h ship too, and lhat.h does not reach them: a
# host that colours L^ in an editor names the lexer itself, and one that only
# runs bytecode never sees it. That is why install_smoke still says what it
# says -- it builds against lhat.h, which is the running half.
install(FILES include/lhat.h DESTINATION include)
install(DIRECTORY include/lhat/ DESTINATION include/lhat
    FILES_MATCHING PATTERN "*.h")
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/include/lhat/version.h
    DESTINATION include/lhat)

# The package: find_package(lhat CONFIG) answers lhat::lhat and friends.
# tests/install_smoke is the proof the installed tree actually works --
# nothing in the ordinary test run reads the install rules at all.
include(CMakePackageConfigHelpers)
install(EXPORT lhatTargets NAMESPACE lhat:: DESTINATION lib/cmake/lhat)
configure_package_config_file(cmake/lhatConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/lhatConfig.cmake
    INSTALL_DESTINATION lib/cmake/lhat)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/lhatConfigVersion.cmake
    COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/lhatConfig.cmake
              ${CMAKE_CURRENT_BINARY_DIR}/lhatConfigVersion.cmake
    DESTINATION lib/cmake/lhat)

# ---- Tests ----------------------------------------------------------------
if(LHAT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
    if(LHAT_BUILD_LSP)
        add_subdirectory(lsp/tests)
    endif()
endif()
