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
# means adding it to those two lists (and to the lint job's own copy of
# tidy's list in .github/workflows/ci.yml).
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

# The Nether goldens' stored biomes read back as the legacy climate noises.
# A target for the same reason the one above is: its --control, --model and
# --modern arms are claims about agreeing with the library, and a file nothing
# builds can drift out of agreement without a gate noticing.
add_executable(stratum_legacy_goldens_biome_analyze legacy-goldens-biome-analyze.cpp)
target_link_libraries(stratum_legacy_goldens_biome_analyze PRIVATE stratum::core ZLIB::ZLIB)
stratum_configure_target(stratum_legacy_goldens_biome_analyze)

# The golden-region readback. Its decoder lives in a header beside it because
# tests/conformance/vanilla_legacy_goldens_surface_test.cpp runs the SAME
# decoder: a conformance case that re-derived the inversion would be asserting
# its own copy of it, which is the one thing the control exists to rule out.
add_executable(stratum_legacy_goldens_surface_analyze legacy-goldens-surface-analyze.cpp)
target_link_libraries(stratum_legacy_goldens_surface_analyze PRIVATE stratum::core ZLIB::ZLIB)
target_include_directories(stratum_legacy_goldens_surface_analyze PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")
stratum_configure_target(stratum_legacy_goldens_surface_analyze)

# The aquifer analyzers, for the same reason as the three above: each scores
# a probe world through the library's own aquifer code, and a file nothing
# builds can stop agreeing with the library it calls without a gate noticing.
# They are investigation tools rather than tests (each prints its own
# figures), so building them is the gate; tests/conformance holds the
# assertions.
foreach(tool IN ITEMS
        aquifer-barrier-analyze
        aquifer-cells
        aquifer-cost-bench
        aquifer-deepfloor-analyze
        aquifer-fluidtype-analyze
        aquifer-level-analyze
        aquifer-nearsurface-analyze
        aquifer-presets-scout
        aquifer-waterlava-analyze)
    string(REPLACE "-" "_" target "stratum_${tool}")
    add_executable(${target} ${tool}.cpp)
    target_link_libraries(${target} PRIVATE stratum::core ZLIB::ZLIB)
    stratum_configure_target(${target})
endforeach()

# The end-to-end chunk-cost bench (M5): what one PocketMine-MP generation task
# costs a chunk, fill, biomes and the chunkutils2 encode. Its own target rather
# than one of the foreach above because it links ext/'s plain C++ half
# (stratum::pmmp_core, which brings lib/ and lib/mapping/), not lib/ alone: the
# encode is part of what it times. The build configuration is compiled in so a
# run says what it measured; tools/perf-baseline builds it with the release
# preset, and only an optimised build's figures are a measurement.
add_executable(stratum_chunk_cost_bench chunk-cost-bench.cpp)
target_link_libraries(stratum_chunk_cost_bench PRIVATE stratum::pmmp_core)
target_compile_definitions(stratum_chunk_cost_bench PRIVATE
    "STRATUM_BENCH_CONFIG=\"$<CONFIG>\"")
stratum_configure_target(stratum_chunk_cost_bench)

# The legacy-source aquifer probe's readback. It scores each arm through the
# library's own aquifer and through tests/support/fluid_flow.hpp's flow
# classifier, the one the conformance cases use, so it includes tests/.
add_executable(stratum_legacy_aquifer_analyze legacy-aquifer-analyze.cpp)
target_link_libraries(stratum_legacy_aquifer_analyze PRIVATE stratum::core ZLIB::ZLIB)
target_include_directories(stratum_legacy_aquifer_analyze PRIVATE "${PROJECT_SOURCE_DIR}/tests")
stratum_configure_target(stratum_legacy_aquifer_analyze)

# The legacy-source ore-vein probe's readback (legacy-orevein-probe.sh). It
# scores each arm through the library's own vein gate and VeinSource, so it
# is a target for the same reason the aquifer analyzers are.
add_executable(stratum_legacy_orevein_analyze legacy-orevein-analyze.cpp)
target_link_libraries(stratum_legacy_orevein_analyze PRIVATE stratum::core ZLIB::ZLIB)
stratum_configure_target(stratum_legacy_orevein_analyze)

# The surface scan's readings (aquifer-ties-probe.sh). Its scorer reads probe
# regions through the conformance cases' own support headers, and
# tests/conformance/vanilla_aquifer_ties_test.cpp runs the same scorer, so it
# includes tests/ too.
add_executable(stratum_aquifer_ties_analyze aquifer-ties-analyze.cpp)
target_link_libraries(stratum_aquifer_ties_analyze PRIVATE stratum::core ZLIB::ZLIB)
target_include_directories(stratum_aquifer_ties_analyze PRIVATE "${PROJECT_SOURCE_DIR}/tests")
stratum_configure_target(stratum_aquifer_ties_analyze)

# The tie-break's visibility over the golden overworld regions, whole. It
# replays the filler through tests/support/aquifer_tiebreak.hpp, the same
# replay golden_aquifer_tiebreak_test.cpp pins a sample of, so it includes
# tests/ too.
add_executable(stratum_aquifer_tiebreak_analyze aquifer-tiebreak-analyze.cpp)
target_link_libraries(stratum_aquifer_tiebreak_analyze PRIVATE stratum::core ZLIB::ZLIB)
target_include_directories(stratum_aquifer_tiebreak_analyze PRIVATE "${PROJECT_SOURCE_DIR}/tests")
stratum_configure_target(stratum_aquifer_tiebreak_analyze)

if(NOT STRATUM_BUILD_TESTS)
    return()
endif()

# The chunk-cost bench's --compare, hermetically, on dump directories the
# script writes itself: identical, a byte apart, a file on one side only, a
# prefix, and the refusals. A perf change is called output-neutral on this
# comparison's word, so the comparison is tested on its own, without fixtures.
add_test(NAME unit.tools.chunk_cost_bench_compare
    COMMAND "${CMAKE_COMMAND}"
            "-DBENCH=$<TARGET_FILE:stratum_chunk_cost_bench>"
            -DMODE=compare
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/chunk-cost-bench-compare"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run-chunk-cost-bench.cmake"
)
set_tests_properties(unit.tools.chunk_cost_bench_compare PROPERTIES LABELS "unit")

# And on the real pipeline: one overworld and one End chunk, twice, through
# every stage. The bench's encode stage must reproduce encodeChunk (it stops
# otherwise), the two runs' dumps must be identical — a dump that differed run
# to run could prove no change neutral — a different chunk's must not be, and
# the JSON tools/perf-baseline reads must carry the figures. Conformance-
# labelled because it reads the fetched worldgen tree (SPEC §12); it skips,
# visibly, without one.
add_test(NAME conformance.chunk_cost_bench
    COMMAND "${CMAKE_COMMAND}"
            "-DBENCH=$<TARGET_FILE:stratum_chunk_cost_bench>"
            -DMODE=fixtures
            "-DSTRATUM_FIXTURES_DIR=${STRATUM_FIXTURES_DIR}"
            "-DSTRATUM_MINECRAFT_VERSION=${STRATUM_MINECRAFT_VERSION}"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/chunk-cost-bench-dumps"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run-chunk-cost-bench.cmake"
)
set_tests_properties(conformance.chunk_cost_bench PROPERTIES
    LABELS "conformance"
    SKIP_RETURN_CODE 77
    SKIP_REGULAR_EXPRESSION "-- SKIP: "
)

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
    SKIP_REGULAR_EXPRESSION "-- SKIP: "
)

# The same idea for the goldens-biome readback: its --control (the decoder on
# the overworld goldens), --model (the hand-rolled stack and climate chain
# against the library, bit for bit) and --modern (the 27.08% already on record)
# arms exist only inside the analyzer, because the conformance case cannot use
# the library as both the model and the thing being checked.
add_test(NAME conformance.legacy_goldens_biome_control
    COMMAND "${CMAKE_COMMAND}"
            "-DANALYZER=$<TARGET_FILE:stratum_legacy_goldens_biome_analyze>"
            "-DSTRATUM_FIXTURES_DIR=${STRATUM_FIXTURES_DIR}"
            "-DSTRATUM_MINECRAFT_VERSION=${STRATUM_MINECRAFT_VERSION}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run-legacy-goldens-biome-control.cmake"
)
set_tests_properties(conformance.legacy_goldens_biome_control PROPERTIES
    LABELS "conformance"
    SKIP_RETURN_CODE 77
    SKIP_REGULAR_EXPRESSION "-- SKIP: "
)
