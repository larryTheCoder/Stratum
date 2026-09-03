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

# --- The NBT reader against real, vanilla-written files ---------------------
add_executable(stratum_conformance_tests
    golden_heightmaps_test.cpp
    golden_terrain_test.cpp
    golden_terrain_no_aquifer_test.cpp
    golden_regions_test.cpp
    nbt_structures_test.cpp
    vanilla_biomes_test.cpp
    vanilla_blended_probe_test.cpp
    vanilla_climate_test.cpp
    vanilla_density_test.cpp
    vanilla_freeze_test.cpp
    vanilla_inline_noise_test.cpp
    vanilla_pack_test.cpp
    vanilla_settings_test.cpp
    vanilla_weird_scaled_test.cpp
    vanilla_render_test.cpp
)

target_link_libraries(stratum_conformance_tests PRIVATE
    stratum::core
    Catch2::Catch2WithMain
    ZLIB::ZLIB
)

target_compile_definitions(stratum_conformance_tests PRIVATE
    "STRATUM_FIXTURES_DIR=\"${STRATUM_FIXTURES_DIR}\""
)

stratum_configure_target(stratum_conformance_tests)

catch_discover_tests(stratum_conformance_tests
    TEST_PREFIX "conformance."
    PROPERTIES LABELS "conformance"
)
