# Each test links the lsp/ source file(s) it exercises directly against
# lhat and cjson, the same shape tests/CMakeLists.txt uses for lhat itself --
# these check code inside the language server, not lhatls.exe as a subprocess.

function(lhat_lsp_add_test name)
    add_executable(${name} ${name}.c ${ARGN})
    # lhatthread は lsp/*.h が port/thread.h の型を持つから要る -- 直接呼ぶ
    # テストは今のところないが、リンクせずに含めることはできない。
    target_link_libraries(${name} PRIVATE lhat cjson lhatthread)
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/tests
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/lsp
    )
    if(NOT MSVC)
        target_link_libraries(${name} PRIVATE m)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

lhat_lsp_add_test(test_uri ${CMAKE_SOURCE_DIR}/lsp/uri.c)
lhat_lsp_add_test(test_diagnostics
    ${CMAKE_SOURCE_DIR}/lsp/diagnostics.c
    ${CMAKE_SOURCE_DIR}/lsp/position.c
)
