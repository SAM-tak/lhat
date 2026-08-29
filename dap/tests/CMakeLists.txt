# Each test links the dap/ source it exercises against the shared transport
# and cjson, the same shape lsp/tests uses -- these check the code inside the
# adapter over a memory stream, not lhat.exe --dap as a subprocess.

function(lhat_dap_add_test name)
    add_executable(${name} ${name}.c ${ARGN})
    target_link_libraries(${name} PRIVATE lhat cjson lhattransport lhat_testkit)
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/dap
    )
    if(NOT MSVC)
        target_link_libraries(${name} PRIVATE m)
    endif()
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS dap TIMEOUT 60)
endfunction()

lhat_dap_add_test(test_dap_protocol ${CMAKE_SOURCE_DIR}/dap/protocol.c)
