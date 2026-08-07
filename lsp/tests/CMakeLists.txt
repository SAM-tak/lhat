# Each test links the lsp/ source file(s) it exercises directly against
# lhat and cjson, the same shape tests/CMakeLists.txt uses for lhat itself --
# these check code inside the LSP server, not lhat-lsp.exe as a subprocess.

function(lhat_lsp_add_test name)
    add_executable(${name} ${name}.c ${ARGN})
    target_link_libraries(${name} PRIVATE lhat cjson)
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
lhat_lsp_add_test(test_diagnostics ${CMAKE_SOURCE_DIR}/lsp/diagnostics.c)
