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
        "      tools/fetch-vanilla --version ${STRATUM_MINECRAFT_VERSION}\n"
        "  (tools/fetch-vanilla is Milestone M1 and does not exist yet.)")
    message(STATUS "SKIP: no fixtures")
    # 77 is this test's SKIP_RETURN_CODE.
    cmake_language(EXIT 77)
endif()

message(STATUS "conformance fixtures present at '${STRATUM_FIXTURES_DIR}'")
