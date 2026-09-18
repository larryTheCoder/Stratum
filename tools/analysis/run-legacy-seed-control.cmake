# Runs legacy-seed-analyze --control over every legacy-seed probe world.
#
# The analyzer's --control carries two arms that exist nowhere else in the
# committed apparatus: the hand-rolled forward model (`layoutFor` +
# `sampleNormal`) against the library's NormalNoise bit for bit, and
# `layoutFor`'s hand-rolled valueFactor against NormalNoise::valueFactor().
# The conformance case in tests/ cannot carry them, because there the library
# IS the model. Without this test they were only ever measured by hand.
#
# Both probe worlds or neither. One world seed agreeing with an RNG-driven
# derivation is evidence that it agrees once, so a partial fixture set is a
# skip that names the world that is missing, not a pass over half the
# evidence.
#
# Missing fixtures exit 77 — this test's SKIP_RETURN_CODE. They are
# Mojang-derived and never committed (SPEC §12).

set(SEEDS 42 31337)
set(PROBES "${STRATUM_FIXTURES_DIR}/${STRATUM_MINECRAFT_VERSION}/probes")

set(MISSING "")
foreach(SEED IN LISTS SEEDS)
    if(NOT IS_DIRECTORY "${PROBES}/legseed_s${SEED}")
        list(APPEND MISSING "${SEED}")
    endif()
endforeach()

if(MISSING)
    string(JOIN ", " MISSING_TEXT ${MISSING})
    string(JOIN ", " SEEDS_TEXT ${SEEDS})
    message(STATUS
        "legacy-seed probe world(s) for seed(s) ${MISSING_TEXT} not found under\n"
        "  '${PROBES}'. These are Mojang-derived and never committed "
        "(SPEC §12).\n"
        "  Generate them with:\n"
        "      tools/analysis/legacy-seed-probe.sh --accept-eula <seed>\n"
        "  for each of: ${SEEDS_TEXT}")
    message(STATUS "SKIP: no legacy-seed probe worlds")
    cmake_language(EXIT 77)
endif()

foreach(SEED IN LISTS SEEDS)
    message(STATUS "legacy-seed control, seed ${SEED}")
    execute_process(
        COMMAND "${ANALYZER}" "${PROBES}/legseed_s${SEED}" "${SEED}" --control
        RESULT_VARIABLE STATUS
        OUTPUT_VARIABLE OUT
        ERROR_VARIABLE ERR
    )
    message(STATUS "${OUT}")
    if(NOT STATUS EQUAL 0)
        message(FATAL_ERROR
            "legacy-seed control FAILED at seed ${SEED} (exit ${STATUS}):\n${ERR}\n"
            "A rule this repository already validates was not recovered through the "
            "scan's own readback and forward model, so no refutation drawn from the "
            "scan means anything. See SPEC §11.")
    endif()
endforeach()

string(JOIN ", " SEEDS_TEXT ${SEEDS})
message(STATUS "legacy-seed control passed on all of: ${SEEDS_TEXT}")
