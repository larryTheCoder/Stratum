# tests/ — Catch2 v3 unit suites plus the conformance harness driver.
#
# Test labels drive the presets named in CLAUDE.md:
#   unit         fast, hermetic, no fixtures        -> ctest --preset dev
#   lint         repo policy checks (determinism)   -> ctest --preset dev
#   conformance  golden diffs vs vanilla output     -> ctest --preset conformance

option(STRATUM_FETCH_CATCH2
    "Fetch Catch2 v3 with FetchContent when it is not installed" ON)

set(STRATUM_CATCH2_TAG "v3.16.0" CACHE STRING "Pinned Catch2 release tag")
set(STRATUM_CATCH2_SHA256
    "0957cae5821b17ce07f0833aaa52b5137643a8382203221f363a8303c109af34"
    CACHE STRING "SHA-256 of the pinned Catch2 release archive")

find_package(Catch2 3 QUIET)

if(NOT Catch2_FOUND)
    if(NOT STRATUM_FETCH_CATCH2)
        message(FATAL_ERROR
            "Catch2 v3 was not found and STRATUM_FETCH_CATCH2=OFF. Install "
            "Catch2 >= 3, or configure with -DSTRATUM_FETCH_CATCH2=ON (needs "
            "network access), or disable tests with -DSTRATUM_BUILD_TESTS=OFF.")
    endif()

    include(FetchContent)
    # A release archive rather than a git clone: a cloned dependency keeps a
    # git update step that can re-run during the build and leave the source
    # tree momentarily incomplete, which showed up here as builds failing on
    # a Catch2 source file that plainly existed. An archive is extracted once
    # and stamped, and its hash is verified — which suits a project whose
    # whole point is reproducibility.
    FetchContent_Declare(Catch2
        URL "https://github.com/catchorg/Catch2/archive/refs/tags/${STRATUM_CATCH2_TAG}.tar.gz"
        URL_HASH SHA256=${STRATUM_CATCH2_SHA256}
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")

    # Our warning set is deliberately aggressive (-Wconversion, -Wold-style-cast,
    # ... ) and CI builds with -Werror. Third-party headers must not be able to
    # fail our build, so they are consumed as system includes.
    foreach(_catch_target Catch2 Catch2WithMain)
        if(TARGET ${_catch_target})
            get_target_property(_catch_includes ${_catch_target}
                INTERFACE_INCLUDE_DIRECTORIES)
            if(_catch_includes)
                set_target_properties(${_catch_target} PROPERTIES
                    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_catch_includes}")
            endif()
        endif()
    endforeach()
endif()

include(Catch)

add_subdirectory(unit)
add_subdirectory(conformance)

# ---------------------------------------------------------------------------
# Repo policy lint. Checks the determinism rules that a compiler cannot see:
# banned RNGs, banned float flags, raw % / >> on signed values.
# ---------------------------------------------------------------------------
if(UNIX)
    # fetch-vanilla's offline behaviour: extraction, provenance and its
    # refusals. Hermetic — the network paths belong to the conformance job.
    add_test(NAME unit.tools.fetch_vanilla
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tools/fetch_vanilla_offline_test.sh"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.."
    )
    set_tests_properties(unit.tools.fetch_vanilla PROPERTIES LABELS "unit")

    add_test(NAME lint.determinism
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/../tools/lint/check-determinism.sh"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.."
    )
    set_tests_properties(lint.determinism PROPERTIES LABELS "lint")
endif()
