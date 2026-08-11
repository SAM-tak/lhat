cmake_minimum_required(VERSION 3.25)

project(lhat
    VERSION 0.1.0
    DESCRIPTION "L^ - a bytecode interpreted glue programming language"
    LANGUAGES C
)

option(LHAT_BUILD_TESTS "Build the L^ test suite" ON)
option(LHAT_BUILD_CLI "Build the lhat command line driver" ON)
option(LHAT_BUILD_LSP "Build the L^ language server" ON)

# 01 の 6.4: comments are kept and attached to the tree, for the language
# server to show and the visual editor to edit. Nothing in the language reads
# them, so a host that embeds the language and no tooling turns this off and
# pays for neither the table nor the walk that attaches it.
option(LHAT_WITH_COMMENTS "Keep comments and attach them to the syntax tree" ON)

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
    # 01 の 10 章 の数値リテラル文法。02 の 14.17改2 の tonumber も読むため、
    # 字句解析器から切り離してある——バイトコードだけを実行するビルドは
    # これを持ち、lexer.c 以降を持たない。
    src/number.c
    src/lexer.c
    src/ast.c
    src/parser.c
    src/type.c
    src/check.c
    src/program.c
    src/code.c
    src/compile.c
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

# PUBLIC: it changes the shape of LhatLexer and LhatNode, so everything
# compiled against these headers has to agree on it.
if(LHAT_WITH_COMMENTS)
    target_compile_definitions(lhat PUBLIC LHAT_WITH_COMMENTS)
endif()

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

# ---- Threads --------------------------------------------------------------
# port/thread.h の頭のコメントの通り、これはコアが求めるものではない -- 言語は
# 単一スレッドで走る。コアの脇に建つもの(stdlib/thread.c と lsp/)だけが要る
# ので、lhatport とは別ターゲットにして `lhat` にはリンクしない。C11 の
# <threads.h> は使わない: macOS には存在せず、MSVC は VS2022 17.8 以降にしか
# ない。pthread と Win32 同期 API のどちらもそれより古くから在る。
find_package(Threads REQUIRED)

add_library(lhatthread port/thread.c)
target_link_libraries(lhatthread PUBLIC Threads::Threads)

# "port/thread.h" と書いて含める -- ヘッダ名だけを置くと stdlib/thread.h と
# 衝突する。下の Driver セクションが stdlib/*.h に対して使っているのと同じ手。
target_include_directories(lhatthread PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# ---- Sample standard library ------------------------------------------------
# stdlib/ の隔離は port/ と非対称: port はコアが既定でリンクする既定実装で、
# stdlib はホストが選んで足す任意のライブラリ。lhat_lsp はこれをリンクしない
# -- 丸ごと消せるだけでなく、最初から組み込まれてすらいない、というのが
# 「サンプル」の意図をより厳密に体現する形。lhat_cli はサンプルドライバとして
# 下の Driver セクションで LHAT_BUILD_STDLIB が ON の間だけこれをリンクする。
option(LHAT_BUILD_STDLIB "Build the L^ sample standard library (stdlib/)" ON)

if(LHAT_BUILD_STDLIB)
    add_library(lhatstdlib
        stdlib/debug.c
        stdlib/error.c
        stdlib/io.c
        stdlib/math.c
        stdlib/random.c
        stdlib/thread.c
    )
    # lhat.h だけで完結する(src/ への private path は要らない) -- port/ と
    # 同じ理由で、これ自体が「lhat.h だけを使って書けるサンプル」の実例
    # になっている。std.thread だけが例外で、OS スレッドは lhat.h の外なので
    # port/thread.h を足す。stdlib/*.h には現れないので PRIVATE。
    target_include_directories(lhatstdlib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(lhatstdlib PUBLIC lhat PRIVATE lhatthread)

    install(TARGETS lhatstdlib
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin)
    install(FILES stdlib/debug.h stdlib/error.h stdlib/io.h stdlib/random.h
            stdlib/thread.h
        DESTINATION include/lhatstdlib)
endif()

# ---- Driver ---------------------------------------------------------------
if(LHAT_BUILD_CLI)
    add_executable(lhat_cli cli/main.c)
    set_target_properties(lhat_cli PROPERTIES OUTPUT_NAME lhat)
    target_link_libraries(lhat_cli PRIVATE lhat)
    # サンプルドライバとして、stdlib/ が建っているならそれを丸ごと登録する
    # (cli/main.c の bind_host_names 参照)。LHAT_BUILD_STDLIB=OFF でも
    # cli/main.c 自体はこのマクロなしでビルドできる -- stdlib は常に任意。
    if(LHAT_BUILD_STDLIB)
        target_link_libraries(lhat_cli PRIVATE lhatstdlib)
        # stdlib/*.h 同士は同じディレクトリにある前提で "error.h" のように
        # 相互 include している (stdlib/io.c 参照) -- cli/main.c だけが
        # よそから見るので、リポジトリルートを足して "stdlib/foo.h" と書く。
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

install(TARGETS lhat lhatport lhatthread
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

# lhatstdlib.lib は lhat_thread_* を参照したまま出荷されるので、それを解ける
# ライブラリとヘッダも一緒に置く。含める側が書く名前は "port/thread.h" のまま
# になるよう、ディレクトリごと据える。
install(FILES port/thread.h DESTINATION include/port)

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
