# ---- The Godot side of L^ --------------------------------------------------
# A GDExtension is a shared library the engine loads at run time and asks for
# classes. It is written against godot-cpp, so this directory is the one part
# of the tree that is C++ -- the language stays C11 and is reached from here
# through include/lhat.h, the same surface any other host names (05 の 8.7).
#
# Two things follow from that and shape everything below:
#
#   * godot-cpp is a dependency from outside the tree, and a large one: it
#     generates its bindings from extension_api.json before it compiles. It is
#     fetched once into a directory shared by every preset rather than per
#     build tree.
#   * the engine decides which file to load from its own name, so the library
#     is named the way godot-cpp names things and is written straight into the
#     demo project's bin/, where demo/lhat.gdextension already points.

enable_language(CXX)

set(LHAT_GODOT_CPP_DIR
    "${CMAKE_SOURCE_DIR}/build/godot-cpp"
    CACHE PATH
    "Where godot-cpp lives. Fetched there unless it is a checkout already."
)
set(LHAT_GODOT_CPP_TAG
    "godot-4.5-stable"
    CACHE STRING
    "godot-cpp tag to fetch. Matches the Godot version the extension loads in."
)

# Fetching is only ever how the directory comes to exist; what is built is
# always the checkout that is there. FetchContent would put the two cases in
# different binary directories, and moving between them rebuilds two thousand
# generated files for no reason.
if(NOT EXISTS "${LHAT_GODOT_CPP_DIR}/CMakeLists.txt")
    message(STATUS "godot-cpp: fetching ${LHAT_GODOT_CPP_TAG} into ${LHAT_GODOT_CPP_DIR}")
    find_package(Git REQUIRED)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} clone --depth 1 --branch ${LHAT_GODOT_CPP_TAG}
                https://github.com/godotengine/godot-cpp.git
                "${LHAT_GODOT_CPP_DIR}"
        RESULT_VARIABLE fetched
    )
    if(NOT fetched EQUAL 0)
        message(FATAL_ERROR
            "could not fetch godot-cpp ${LHAT_GODOT_CPP_TAG}. Point "
            "LHAT_GODOT_CPP_DIR at a checkout, or clone one there yourself.")
    endif()
endif()

# An existing checkout is used as it stands -- that is what pointing
# LHAT_GODOT_CPP_DIR somewhere else is for, and the version there is the
# version the extension is built against, tag or no tag.
message(STATUS "godot-cpp: building the checkout at ${LHAT_GODOT_CPP_DIR}")
add_subdirectory("${LHAT_GODOT_CPP_DIR}" godot-cpp)

# ---- What linking a static C library into a shared library asks for --------
# Both of these are about `lhat` having been defined long before godot-cpp was
# read, so neither could have been settled where the targets are made.
set_property(TARGET lhat lhatport PROPERTY POSITION_INDEPENDENT_CODE ON)

# godot-cpp links the MSVC runtime statically by default (GODOTCPP_USE_STATIC_CPP),
# and the linker refuses a mix. Its choice is the one that reaches the engine,
# so the language follows it rather than the other way round -- which is also
# why the godot-* presets build the extension alone, with no cli, server or
# tests in the same tree to disagree.
if(MSVC)
    set_property(TARGET lhat lhatport
        PROPERTY MSVC_RUNTIME_LIBRARY "${CMAKE_MSVC_RUNTIME_LIBRARY}")
endif()

# ---- The extension ---------------------------------------------------------
add_library(lhat_gdextension SHARED
    src/register_types.cpp
    src/lhat_host.cpp
    src/lhat_godot_module.cpp
    src/lhat_variant.cpp
    src/lhat_runtime.cpp
    src/lhat_script.cpp
    src/lhat_instance.cpp
    src/lhat_language.cpp
    src/lhat_highlighter.cpp
    src/lhat_resource_format.cpp
)

target_link_libraries(lhat_gdextension PRIVATE lhat godot-cpp)

# LhatString ends in a flexible array member, which C has and C++ does not --
# every compiler here takes it, and each has its own way of being asked to stop
# saying so.
if(MSVC)
    target_compile_options(lhat_gdextension PRIVATE /wd4200)
else()
    target_compile_options(lhat_gdextension PRIVATE -Wno-pedantic)
endif()

# The engine reads the platform, the Godot target and the architecture out of
# the file name, and godot-cpp already assembles that suffix for its own
# library. Taking it from there is what keeps demo/lhat.gdextension right.
get_target_property(LHAT_GD_SUFFIX godot-cpp GODOTCPP_SUFFIX)
set(LHAT_GD_BIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/demo/bin")

set_target_properties(lhat_gdextension PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET ${GODOTCPP_SYMBOL_VISIBILITY}

    POSITION_INDEPENDENT_CODE ON
    BUILD_RPATH_USE_ORIGIN ON

    # Wrapped in a generator expression so that the multi-config generators do
    # not add a configuration directory of their own underneath.
    LIBRARY_OUTPUT_DIRECTORY "$<1:${LHAT_GD_BIN_DIR}>"
    RUNTIME_OUTPUT_DIRECTORY "$<1:${LHAT_GD_BIN_DIR}>"
    ARCHIVE_OUTPUT_DIRECTORY "$<1:${LHAT_GD_BIN_DIR}>"
    PDB_OUTPUT_DIRECTORY "$<1:${LHAT_GD_BIN_DIR}>"

    PREFIX "lib"
    OUTPUT_NAME "lhat${LHAT_GD_SUFFIX}"
)
