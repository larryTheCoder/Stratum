# tests/conformance/ — Tier-A golden diffs against vanilla output (SPEC §7).
#
# Fixtures are Mojang-derived and are NEVER committed (SPEC §12): they are
# produced locally or in CI by tools/fetch-vanilla from an official jar the
# user downloads themselves. The repo ships the scripts, not the data.
#
# M0 wires the label, the fixture location and the guard. The real cases
# (cli diff over a fixed seed set) arrive with the harness in M1.

set(STRATUM_FIXTURES_DIR "${CMAKE_SOURCE_DIR}/.fixtures" CACHE PATH
    "Location of locally generated, never-committed vanilla fixtures")

add_test(NAME conformance.fixtures_present
    COMMAND "${CMAKE_COMMAND}"
            "-DSTRATUM_FIXTURES_DIR=${STRATUM_FIXTURES_DIR}"
            "-DSTRATUM_MINECRAFT_VERSION=${STRATUM_MINECRAFT_VERSION}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/fixtures_guard.cmake"
)
set_tests_properties(conformance.fixtures_present PROPERTIES
    LABELS "conformance"
    SKIP_RETURN_CODE 77
)
