# Installs the current build tree into a scratch prefix, then configures,
# builds and runs tests/install_smoke against it through find_package.
# Driven as: cmake -DBUILD_DIR=... -DSOURCE_DIR=... -P install_smoke.cmake
#
# This is the only place install breakage becomes visible: nothing in the
# ordinary test run reads the install rules at all.

if(NOT DEFINED BUILD_DIR OR NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "BUILD_DIR and SOURCE_DIR are required")
endif()

set(prefix ${BUILD_DIR}/install_smoke/prefix)
set(hostbuild ${BUILD_DIR}/install_smoke/host)
file(REMOVE_RECURSE ${prefix} ${hostbuild})

execute_process(
    COMMAND ${CMAKE_COMMAND} --install ${BUILD_DIR} --prefix ${prefix}
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "cmake --install failed")
endif()

execute_process(
    COMMAND ${CMAKE_COMMAND}
        -S ${SOURCE_DIR}/tests/install_smoke -B ${hostbuild}
        -G Ninja -DCMAKE_BUILD_TYPE=Debug
        -DCMAKE_PREFIX_PATH=${prefix}
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "configuring the host against the install failed")
endif()

execute_process(
    COMMAND ${CMAKE_COMMAND} --build ${hostbuild}
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "building the host against the install failed")
endif()

execute_process(
    COMMAND ${hostbuild}/host
    OUTPUT_VARIABLE said
    RESULT_VARIABLE result)
if(NOT result EQUAL 0 OR NOT said MATCHES "answers 42")
    message(FATAL_ERROR "the installed host did not answer 42: ${said}")
endif()
message(STATUS "install smoke: ok")
