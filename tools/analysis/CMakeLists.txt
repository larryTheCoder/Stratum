# tools/analysis/ — investigation tools built on lib/, under the build.
#
# WHY THESE ARE A CMAKE TARGET AT ALL. They are not shipped and nothing links
# them, so for a long time they lived outside the build entirely, compiled by
# hand from a g++ line in a file header. That is how a claim like "the
# analyzer no longer normalises independently of the library" becomes
# unfalsifiable: the file it is about is in no target, so nothing in
# tools/lint/format.sh, tools/lint/warnings.sh or tools/lint/tidy.sh ever
# looks at it, and it can drift from the library it is claimed to agree with
# without a single gate turning red.
#
# Being a target fixes that at the root. The file now builds under the project
# warning set (warnings.sh is a -Werror build of everything), and format.sh
# and tidy.sh name it explicitly -- they list the analysis sources that are
# build targets rather than sweeping tools/analysis, the rest of which is
# one-shot investigation code nothing compiles. Adding another target here
# means adding it to those two lists.
#
# The test below is the other half: it RUNS the analyzer's --control, which is
# where the hand-rolled forward model and its valueFactor are compared against
# the library bit for bit. Those two arms exist nowhere else — the conformance
# case in tests/conformance/vanilla_legacy_seed_control_test.cpp uses the
# library AS the model, so it has nothing to compare.
#
# Its own option rather than STRATUM_BUILD_TESTS, because tools/lint/tidy.sh
# configures with tests OFF and still needs these in compile_commands.json.

option(STRATUM_BUILD_ANALYSIS "Build the tools/analysis/ investigation tools" ON)

if(NOT STRATUM_BUILD_ANALYSIS)
    return()
endif()

add_executable(stratum_legacy_seed_analyze legacy-seed-analyze.cpp)
target_link_libraries(stratum_legacy_seed_analyze PRIVATE stratum::core ZLIB::ZLIB)
stratum_configure_target(stratum_legacy_seed_analyze)

if(NOT STRATUM_BUILD_TESTS)
    return()
endif()

# Conformance-labelled because it reads probe worlds, which are Mojang-derived
# and never committed (SPEC §12). It exits 77 — CTest's skip code — when they
# are absent, so a checkout without fixtures gets a visible skip rather than a
# green run that checked nothing.
add_test(NAME conformance.legacy_seed_control_tool
    COMMAND "${CMAKE_COMMAND}"
            "-DANALYZER=$<TARGET_FILE:stratum_legacy_seed_analyze>"
            "-DSTRATUM_FIXTURES_DIR=${STRATUM_FIXTURES_DIR}"
            "-DSTRATUM_MINECRAFT_VERSION=${STRATUM_MINECRAFT_VERSION}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run-legacy-seed-control.cmake"
)
set_tests_properties(conformance.legacy_seed_control_tool PROPERTIES
    LABELS "conformance"
    SKIP_RETURN_CODE 77
)
