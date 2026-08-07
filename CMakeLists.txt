cmake_minimum_required(VERSION 3.25)

project(lhat
    VERSION 0.1.0
    DESCRIPTION "L^ - a bytecode interpreted glue programming language"
    LANGUAGES C
)

option(LHAT_BUILD_TESTS "Build the L^ test suite" ON)
option(LHAT_BUILD_CLI "Build the lhat command line driver" ON)
option(LHAT_BUILD_LSP "Build the L^ language server" ON)

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
    src/gc.c
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

target_include_directories(lhatport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# So that a replacement of these files, written against include/lhat.h alone,
# compiles the same way ours does.
target_include_directories(lhatport PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# The default is what anyone linking the language gets unless they say
# otherwise. PUBLIC so it carries through to whatever links lhat.
target_link_libraries(lhat PUBLIC lhatport)

# ---- Sample standard library ------------------------------------------------
# std/ の隔離は port/ と非対称: port はコアが既定でリンクする既定実装で、
# std はホストが選んで足す任意のライブラリ。lhat_cli/lhat_lsp はどちらも
# 既定でこれをリンクしない -- 丸ごと消せるだけでなく、最初から組み込まれて
# すらいない、というのが「サンプル」の意図をより厳密に体現する形。
option(LHAT_BUILD_STD "Build the L^ sample standard library (std/)" ON)

if(LHAT_BUILD_STD)
    # std.thread が新規 OS スレッドを起動するためだけに要る。std.io/
    # std.random はリンクするだけでこれに巻き込まれる -- 3つを別ライブラリ
    # に分けるほどの理由はなく、port/ と同様ひとまとまりのターゲットにする。
    find_package(Threads REQUIRED)

    add_library(lhatstd
        std/io.c
        std/random.c
        std/thread.c
    )
    # lhat.h だけで完結する(src/ への private path は要らない) -- port/ と
    # 同じ理由で、これ自体が「lhat.h だけを使って書けるサンプル」の実例
    # になっている。
    target_include_directories(lhatstd PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(lhatstd PUBLIC lhat PRIVATE Threads::Threads)

    install(TARGETS lhatstd
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin)
    install(FILES std/io.h std/random.h std/thread.h DESTINATION include/lhatstd)
endif()

# ---- Driver ---------------------------------------------------------------
if(LHAT_BUILD_CLI)
    add_executable(lhat_cli cli/main.c)
    set_target_properties(lhat_cli PROPERTIES OUTPUT_NAME lhat)
    target_link_libraries(lhat_cli PRIVATE lhat)
    install(TARGETS lhat_cli RUNTIME DESTINATION bin)
endif()

# ---- Language server --------------------------------------------------------
# A second, independent consumer of lhat.lib alongside the CLI -- it speaks
# LSP over stdio instead of a prompt. vendor/cjson is JSON-RPC's message
# shape; nothing about lhat's own build needs it, so it is scoped to here.
if(LHAT_BUILD_LSP)
    add_library(cjson STATIC vendor/cjson/cJSON.c)
    target_include_directories(cjson PUBLIC vendor/cjson)
    if(MSVC)
        target_compile_options(cjson PRIVATE /W0)
    else()
        target_compile_options(cjson PRIVATE -w)
    endif()

    find_package(Threads REQUIRED)

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
        lsp/diagnostics.c
        lsp/queue.c
        lsp/handlers/initialize.c
        lsp/handlers/text_document_sync.c
    )
    set_target_properties(lhat_lsp PROPERTIES OUTPUT_NAME lhat-lsp)
    target_link_libraries(lhat_lsp PRIVATE lhat cjson Threads::Threads)
    target_include_directories(lhat_lsp PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/lsp
    )
    install(TARGETS lhat_lsp RUNTIME DESTINATION bin)
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

# machine.h is the inside of LhatMachine and gc.h is what reads it. vm.h keeps
# the machine opaque on purpose, and nothing lhat.h reaches includes either of
# these -- so they are the machine's own business and are not shipped.
install(DIRECTORY src/ DESTINATION src
    FILES_MATCHING PATTERN "*.h"
    PATTERN "machine.h" EXCLUDE
    PATTERN "gc.h" EXCLUDE)

# ---- Tests ----------------------------------------------------------------
if(LHAT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
    if(LHAT_BUILD_LSP)
        add_subdirectory(lsp/tests)
    endif()
endif()
