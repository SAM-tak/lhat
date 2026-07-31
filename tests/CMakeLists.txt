# Each test file becomes one executable and one ctest entry. The executables
# link against lhat_core directly rather than driving the lhat binary, so unit
# level checks stay possible as the language grows.

function(lhat_add_test name)
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} PRIVATE lhat_core)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    if(NOT MSVC)
        target_link_libraries(${name} PRIVATE m)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

lhat_add_test(test_source)
lhat_add_test(test_lexer)
lhat_add_test(test_parser)
lhat_add_test(test_type)
lhat_add_test(test_check)
lhat_add_test(test_value)
