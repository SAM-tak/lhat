# Each test links the lsp/ source file(s) it exercises directly against
# lhat and cjson, the same shape tests/CMakeLists.txt uses for lhat itself --
# these check code inside the language server, not lhatls.exe as a subprocess.

function(lhat_lsp_add_test name)
    add_executable(${name} ${name}.c ${ARGN})
    # lhatthread is needed because the lsp/*.h carry types from
    # port/thread.h -- no test calls one directly yet, but they cannot be
    # included without linking it.
    target_link_libraries(${name} PRIVATE lhat cjson lhatthread lhat_testkit)
    # The repository root as well: lsp/lton.c and its test name
    # stdlib/lton.h for the wrapper an LTON file is read inside.
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/lsp
    )
    if(NOT MSVC)
        target_link_libraries(${name} PRIVATE m)
    endif()
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS lsp TIMEOUT 60)
endfunction()

lhat_lsp_add_test(test_uri ${CMAKE_SOURCE_DIR}/lsp/uri.c)
lhat_lsp_add_test(test_lton_server
    ${CMAKE_SOURCE_DIR}/lsp/lton.c
    ${CMAKE_SOURCE_DIR}/lsp/position.c
)
lhat_lsp_add_test(test_diagnostics
    ${CMAKE_SOURCE_DIR}/lsp/diagnostics.c
    ${CMAKE_SOURCE_DIR}/lsp/position.c
    ${CMAKE_SOURCE_DIR}/lsp/lton.c
)
lhat_lsp_add_test(test_ast_json ${CMAKE_SOURCE_DIR}/lsp/ast_json.c)
lhat_lsp_add_test(test_hover
    ${CMAKE_SOURCE_DIR}/lsp/hover.c
    ${CMAKE_SOURCE_DIR}/lsp/position.c
    ${CMAKE_SOURCE_DIR}/lsp/lton.c
    ${CMAKE_SOURCE_DIR}/lsp/resolution.c
)
lhat_lsp_add_test(test_host_config ${CMAKE_SOURCE_DIR}/lsp/host_config.c)
lhat_lsp_add_test(test_definition ${CMAKE_SOURCE_DIR}/lsp/definition.c)
lhat_lsp_add_test(test_signature
    ${CMAKE_SOURCE_DIR}/lsp/signature.c
    ${CMAKE_SOURCE_DIR}/lsp/resolution.c
)
lhat_lsp_add_test(test_document_symbol
    ${CMAKE_SOURCE_DIR}/lsp/document_symbol.c
    ${CMAKE_SOURCE_DIR}/lsp/position.c
    ${CMAKE_SOURCE_DIR}/lsp/lton.c
    ${CMAKE_SOURCE_DIR}/lsp/util.c
)
lhat_lsp_add_test(test_semantic_tokens
    ${CMAKE_SOURCE_DIR}/lsp/semantic_tokens.c
    ${CMAKE_SOURCE_DIR}/lsp/position.c
    ${CMAKE_SOURCE_DIR}/lsp/lton.c
)
