# Reports whether the never-committed vanilla fixtures are present.
#
# Missing fixtures are a SKIP with an actionable message (exit 77), not a
# silent pass: a green conformance run that compared nothing would be a lie
# about Tier-A parity.

if(NOT EXISTS "${STRATUM_FIXTURES_DIR}")
    message(STATUS
        "conformance fixtures not found at '${STRATUM_FIXTURES_DIR}'.\n"
        "  These are derived from Mojang data and are never committed "
        "(SPEC §12).\n"
        "  Generate them locally with:\n"
        "      tools/fetch-vanilla --version ${STRATUM_MINECRAFT_VERSION}")
    message(STATUS "SKIP: no fixtures")
    # 77 is this test's SKIP_RETURN_CODE.
    # cmake_language(EXIT) exists only from CMake 3.29, and this project's
    # minimum is 3.24: on 3.28 the bare call was itself an error, so a missing
    # fixture FAILED instead of skipping. Below 3.29 the script just ends, and
    # the test's SKIP_REGULAR_EXPRESSION reads the "SKIP:" line above.
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
        cmake_language(EXIT 77)
    endif()
    return()
endif()

message(STATUS "conformance fixtures present at '${STRATUM_FIXTURES_DIR}'")
