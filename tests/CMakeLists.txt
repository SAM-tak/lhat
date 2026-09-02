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
lhat_add_test(test_serialize core)
lhat_add_test(test_leak core)

# The checker suite, split by topic; each file is one executable.
lhat_add_test(test_check_core check)
lhat_add_test(test_check_instantiation check)
lhat_add_test(test_check_typedef check)
lhat_add_test(test_check_narrowing check)
lhat_add_test(test_check_def check)
lhat_add_test(test_check_forms check)
lhat_add_test(test_check_modules check)
lhat_add_test(test_check_coroutine check)
lhat_add_test(test_check_purity check)
lhat_add_test(test_check_tables check)
lhat_add_test(test_check_novalue check)
lhat_add_test(test_error_local check)

# The machine suite, the same way.
lhat_add_test(test_vm_core vm)
lhat_add_test(test_vm_data vm)
lhat_add_test(test_vm_enum vm)
lhat_add_test(test_vm_loop vm)
lhat_add_test(test_vm_error vm)
lhat_add_test(test_vm_def vm)
lhat_add_test(test_vm_coroutine vm)
lhat_add_test(test_vm_machine vm)
if(LHAT_WITH_DEBUGGER)
    lhat_add_test(test_debug_hook vm)
endif()
lhat_add_test(test_vm_tuple vm)
lhat_add_test(test_host_coroutine vm)
lhat_add_test(test_hostdata_base vm)

# The sample stdlib's tests. They ask what an L^ program sees when it
# imports a module, through the public headers rather than through src/
# (see the head of tests/stdlibutil.h) -- which makes them the acceptance
# test of that surface. Not built when LHAT_BUILD_STDLIB is OFF.
if(LHAT_BUILD_STDLIB)
    # std.thread is the one that starts OS threads itself, so it needs
    # lhatthread as well.
    lhat_add_test(test_thread stdlib)
    target_link_libraries(test_thread PRIVATE lhatstdlib lhatthread)

    # std.async is the other one that needs the port's clock and lock.
    lhat_add_test(test_async stdlib)
    target_link_libraries(test_async PRIVATE lhatstdlib lhatthread)

    lhat_add_test(test_io stdlib)
    target_link_libraries(test_io PRIVATE lhatstdlib)

    lhat_add_test(test_json stdlib)
    target_link_libraries(test_json PRIVATE lhatstdlib)

    lhat_add_test(test_random stdlib)
    target_link_libraries(test_random PRIVATE lhatstdlib)

    lhat_add_test(test_regex stdlib)
    target_link_libraries(test_regex PRIVATE lhatstdlib)
    lhat_add_test(test_load stdlib)
    target_link_libraries(test_load PRIVATE lhatstdlib)

    lhat_add_test(test_lton stdlib)
    target_link_libraries(test_lton PRIVATE lhatstdlib)

    lhat_add_test(test_math stdlib)
    target_link_libraries(test_math PRIVATE lhatstdlib)
    lhat_add_test(test_mathvector3 stdlib)
    target_link_libraries(test_mathvector3 PRIVATE lhatstdlib)

    lhat_add_test(test_debug stdlib)
    target_link_libraries(test_debug PRIVATE lhatstdlib)

    # 05 の 8.7: what a registration keeps for as long as the program does is
    # part of what a host pays on every reload, so the leak test covers the
    # sample library too where there is one to cover.
    target_link_libraries(test_leak PRIVATE lhatstdlib)
    target_compile_definitions(test_leak PRIVATE LHAT_LEAK_WITH_STDLIB=1)
    target_include_directories(test_leak PRIVATE ${CMAKE_SOURCE_DIR}/stdlib)
endif()

# End to end: the cli checks and runs each committed sample; the exit code
# is the whole assertion, since golden output would be brittleness of its
# own. Named files rather than a glob, so a half-written sketch in sample/
# does not fail the suite.
# The installed tree, proven end to end: install into a scratch prefix,
# then configure, build and run tests/install_smoke against it through
# find_package. See scripts/install_smoke.cmake.
add_test(NAME install_smoke
         COMMAND ${CMAKE_COMMAND}
             -DBUILD_DIR=${CMAKE_BINARY_DIR}
             -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
             -P ${CMAKE_SOURCE_DIR}/scripts/install_smoke.cmake)
set_tests_properties(install_smoke PROPERTIES LABELS e2e TIMEOUT 300)

if(LHAT_BUILD_CLI)
    set(lhat_e2e_samples
        vector.lh
        # 02 の 15.14: the async layer end to end -- the scheduler is L^
        # (sample/async.lh, required by each of these), so what these prove is
        # that await^, std.async and std.thread meet where they are supposed
        # to. None reads stdin and each finishes in a fraction of a second.
        asynctest.lh
        asyncpump.lh
        asyncthread.lh
    )
    foreach(sample ${lhat_e2e_samples})
        cmake_path(GET sample STEM stem)
        add_test(NAME e2e_${stem}
                 COMMAND lhat_cli --run ${CMAKE_SOURCE_DIR}/sample/${sample})
        set_tests_properties(e2e_${stem} PROPERTIES LABELS e2e TIMEOUT 60)
    endforeach()
endif()
