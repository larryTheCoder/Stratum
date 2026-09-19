# Runs legacy-seed-analyze --control and --plant-axes over every legacy-seed
# probe world.
#
# The analyzer's --control carries three arms that exist nowhere else in the
# committed apparatus: the hand-rolled forward model (`layoutFor` +
# `sampleNormal`) against the library's NormalNoise bit for bit,
# `layoutFor`'s hand-rolled valueFactor against NormalNoise::valueFactor(),
# and — since the widening — the table-driven evaluator the scan and the
# plants actually run, against `sampleNormal` at its baseline point, bit for
# bit. The conformance case in tests/ cannot carry them, because there the
# library IS the model. Without this test they were only ever measured by hand.
#
# --plant-axes is the second half, and it is what makes a null on the widened
# axes a measurement. It plants a candidate that is off the baseline in each
# new axis — a stack shape, a per-octave salting, a frequency offset, an
# ordinal-seeded rule, and one candidate off the baseline in three axes at
# once — and requires each to come back at RANK 1 from the identical scan. A
# scan that could not find its own plant on an axis has refuted nothing there.
#
# It runs on ONE dimension per world rather than all six, and that is a cost
# decision stated rather than hidden. The five plants already share one pass
# over the space — a plant is the same lattice with different values, so a
# candidate is evaluated once and compared against each — which is what makes
# five of them cost barely more than one. What remains is the pass itself: 57
# million candidates, about 20 seconds optimised and about three and a half
# minutes in the Debug build this preset uses. Measured whole: this test is
# 435 seconds, control and plants, both worlds; all six legacy dimensions
# would be about forty-five minutes of CTest. `leg_skip` is the dimension chosen
# because it is the one whose noise has a ZERO amplitude, so it is the only
# one where the order and zero-consumption sub-axes are not degenerate — 22
# distinct stack shapes there against 13 on the 1-octave dimensions. The full
# six-dimension, two-world run is 60 plants and is recorded in SPEC §11.
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

    message(STATUS "legacy-seed axis plants, seed ${SEED}")
    execute_process(
        COMMAND "${ANALYZER}" "${PROBES}/legseed_s${SEED}" "${SEED}" --plant-axes
                --only leg_skip
        RESULT_VARIABLE PLANT_STATUS
        OUTPUT_VARIABLE PLANT_OUT
        ERROR_VARIABLE PLANT_ERR
    )
    message(STATUS "${PLANT_OUT}")
    if(NOT PLANT_STATUS EQUAL 0)
        message(FATAL_ERROR
            "legacy-seed axis plants FAILED at seed ${SEED} (exit ${PLANT_STATUS}):\n"
            "${PLANT_ERR}\n"
            "A candidate planted inside one of the widened axes was not recovered at rank 1 "
            "by the identical scan, so a null on that axis is not a measurement. See "
            "SPEC §11.")
    endif()
endforeach()

string(JOIN ", " SEEDS_TEXT ${SEEDS})
message(STATUS "legacy-seed control and axis plants passed on all of: ${SEEDS_TEXT}")
