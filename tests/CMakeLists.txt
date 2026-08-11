# Each test file becomes one executable and one ctest entry. The executables
# link against lhat directly rather than driving the lhat binary, so unit
# level checks stay possible as the language grows.
#
# They name the internal headers rather than include/lhat.h, since what they
# check is inside: a lexer, an arena, a type. So src/ is on their path, which
# is the one place outside the library itself that it is.

# The harness's own translation units: counters and the tally in testutil.c,
# the parse/check/run fixtures in fixture.c. Shared by every test executable
# (and by lsp/tests).
add_library(lhat_testkit STATIC testutil.c fixture.c stdlibutil.c)
target_include_directories(lhat_testkit PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lhat_testkit PUBLIC lhat)
target_include_directories(lhat_testkit PRIVATE ${CMAKE_SOURCE_DIR}/src)

# `label` groups the entry for `ctest -L core|check|vm|stdlib|e2e`. Every
# test runs in its own directory under run/, so a case writing scratch
# files leaves the source tree and the other tests alone.
function(lhat_add_test name label)
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} PRIVATE lhat lhat_testkit)
    target_include_directories(${name} PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    if(NOT MSVC)
        target_link_libraries(${name} PRIVATE m)
    endif()
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/run/${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES
        LABELS ${label}
        TIMEOUT 60
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/run/${name}
    )
endfunction()

lhat_add_test(test_source core)
lhat_add_test(test_error core)
lhat_add_test(test_number core)
lhat_add_test(test_lexer core)
lhat_add_test(test_parser core)
lhat_add_test(test_type core)
lhat_add_test(test_value core)
lhat_add_test(test_object core)
lhat_add_test(test_program core)

# The checker suite, split by topic; each file is one executable.
lhat_add_test(test_check_core check)
lhat_add_test(test_check_narrowing check)
lhat_add_test(test_check_def check)
lhat_add_test(test_check_forms check)
lhat_add_test(test_check_modules check)
lhat_add_test(test_check_coroutine check)
lhat_add_test(test_check_purity check)
lhat_add_test(test_check_tables check)
lhat_add_test(test_check_novalue check)

# The machine suite, the same way.
lhat_add_test(test_vm_core vm)
lhat_add_test(test_vm_data vm)
lhat_add_test(test_vm_loop vm)
lhat_add_test(test_vm_error vm)
lhat_add_test(test_vm_def vm)
lhat_add_test(test_vm_coroutine vm)
lhat_add_test(test_vm_machine vm)
lhat_add_test(test_vm_tuple vm)

# サンプル stdlib のテスト。src/ ではなく include/lhat.h 越しに、L^ の
# プログラムから見える形を検査する(tests/stdlibutil.h の頭のコメント)。
# LHAT_BUILD_STDLIB=OFF なら建てない。
if(LHAT_BUILD_STDLIB)
    # std.thread だけは OS スレッドを自分で起動するので lhatthread も要る。
    lhat_add_test(test_thread stdlib)
    target_link_libraries(test_thread PRIVATE lhatstdlib lhatthread)

    lhat_add_test(test_io stdlib)
    target_link_libraries(test_io PRIVATE lhatstdlib)

    lhat_add_test(test_random stdlib)
    target_link_libraries(test_random PRIVATE lhatstdlib)

    lhat_add_test(test_math stdlib)
    target_link_libraries(test_math PRIVATE lhatstdlib)

    lhat_add_test(test_debug stdlib)
    target_link_libraries(test_debug PRIVATE lhatstdlib)
endif()

# End to end: the cli checks and runs each committed sample; the exit code
# is the whole assertion, since golden output would be brittleness of its
# own. Named files rather than a glob, so a half-written sketch in sample/
# does not fail the suite.
if(LHAT_BUILD_CLI)
    set(lhat_e2e_samples
        vector.lh
    )
    foreach(sample ${lhat_e2e_samples})
        cmake_path(GET sample STEM stem)
        add_test(NAME e2e_${stem}
                 COMMAND lhat_cli --run ${CMAKE_SOURCE_DIR}/sample/${sample})
        set_tests_properties(e2e_${stem} PROPERTIES LABELS e2e TIMEOUT 60)
    endforeach()
endif()
