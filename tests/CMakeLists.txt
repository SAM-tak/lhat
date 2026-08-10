# Each test file becomes one executable and one ctest entry. The executables
# link against lhat directly rather than driving the lhat binary, so unit
# level checks stay possible as the language grows.
#
# They name the internal headers rather than include/lhat.h, since what they
# check is inside: a lexer, an arena, a type. So src/ is on their path, which
# is the one place outside the library itself that it is.

function(lhat_add_test name)
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} PRIVATE lhat)
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src
    )
    if(NOT MSVC)
        target_link_libraries(${name} PRIVATE m)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

lhat_add_test(test_source)
lhat_add_test(test_error)
lhat_add_test(test_lexer)
lhat_add_test(test_parser)
lhat_add_test(test_type)
lhat_add_test(test_check)
lhat_add_test(test_value)
lhat_add_test(test_object)
lhat_add_test(test_program)
lhat_add_test(test_vm)

# サンプル stdlib のテスト。src/ ではなく include/lhat.h 越しに、L^ の
# プログラムから見える形を検査する(tests/stdlibutil.h の頭のコメント)。
# LHAT_BUILD_STDLIB=OFF なら建てない。
if(LHAT_BUILD_STDLIB)
    # std.thread だけは OS スレッドを自分で起動するので lhatthread も要る。
    lhat_add_test(test_thread)
    target_link_libraries(test_thread PRIVATE lhatstdlib lhatthread)

    lhat_add_test(test_io)
    target_link_libraries(test_io PRIVATE lhatstdlib)

    lhat_add_test(test_random)
    target_link_libraries(test_random PRIVATE lhatstdlib)

    lhat_add_test(test_math)
    target_link_libraries(test_math PRIVATE lhatstdlib)

    lhat_add_test(test_debug)
    target_link_libraries(test_debug PRIVATE lhatstdlib)
endif()
