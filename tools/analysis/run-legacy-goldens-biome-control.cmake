# Runs legacy-goldens-biome-analyze's control arms against the golden regions.
#
# Three of this analyzer's claims cannot live in tests/conformance, because
# there the library IS the model and there is nothing to compare:
#
#   --control  the decode that the scan's conclusion rests on, run on the
#              OVERWORLD goldens where the seeding is the modern one this build
#              reproduces: a known-correct rule at ~100%, the same rule at
#              worldSeed + 1 at the null, and the null itself measured as
#              vanilla-against-vanilla over every pair of the eight worlds.
#              All three of its figures are matched as text below, so the
#              SPEC §11 control table cannot drift without this failing.
#   --model    the hand-rolled NormalNoise stack and the hand-rolled
#              shift/shifted_noise chain against noise::NormalNoise and
#              density::Interpreter, bit for bit, for the three noises actually
#              in play. A wrong valueFactor, a missing second stack or a
#              misread of shift_b's axis rotation would otherwise be shared
#              silently between the scan and its own null.
#   --modern   the 8873/32768 = 27.08% against a 30.23% baseline that
#              tests/conformance/vanilla_legacy_nether_climate_gap_test.cpp
#              already measured for the modern derivation, reproduced through
#              THIS analyzer's chain. Agreeing with a number produced by
#              entirely different code is the sanity check that says the chain
#              is reading the Nether and not something else.
#
# --candidate 182 0 is here for a fourth reason: rule 182 is deepslate's own
# derivation, and BOTH analyzers have to agree on what rule index 182 means, or
# a claim carried from one file to the other is about different things. The
# string checked below is the one legacy-seed-analyze.cpp's header names.
#
# The full --scan is deliberately NOT run: 270,000 candidates is minutes of
# compute, and its result is recorded in SPEC §11 with the command that
# produces it. Nor are --split (a second scan of the same size) or --null-full
# all (the same space at the full denominator, about an hour); those two are
# recorded the same way.
#
# Missing fixtures exit 77 — this test's SKIP_RETURN_CODE. They are
# Mojang-derived and never committed (SPEC §12).

set(REGIONS "${STRATUM_FIXTURES_DIR}/${STRATUM_MINECRAFT_VERSION}/regions")
set(SEEDS 0 1 -1 42 2891948927356891 -4172144997902289642
          9223372036854775807 -9223372036854775808)

set(MISSING "")
foreach(SEED IN LISTS SEEDS)
    foreach(DIMENSION nether overworld)
        if(NOT EXISTS "${REGIONS}/seed-${SEED}/${DIMENSION}/r.0.0.mca")
            list(APPEND MISSING "seed-${SEED}/${DIMENSION}")
        endif()
    endforeach()
endforeach()

if(MISSING)
    string(JOIN ", " MISSING_TEXT ${MISSING})
    message(STATUS
        "golden region(s) ${MISSING_TEXT} not found under\n"
        "  '${REGIONS}'. These are Mojang-derived and never committed "
        "(SPEC §12).\n"
        "  Generate them with:\n"
        "      tools/fetch-vanilla --generate-regions --accept-eula")
    message(STATUS "SKIP: no golden regions")
    cmake_language(EXIT 77)
endif()

# All eight or none. Six of them are the six independent worlds and two are
# collisions of the other six; a partial set would silently change every
# denominator the analyzer prints.
foreach(ARM --control --model --modern)
    message(STATUS "legacy-goldens-biome ${ARM}")
    execute_process(
        COMMAND "${ANALYZER}" "${STRATUM_FIXTURES_DIR}" "${ARM}"
        RESULT_VARIABLE STATUS
        OUTPUT_VARIABLE OUT
        ERROR_VARIABLE ERR
    )
    message(STATUS "${OUT}")
    if(NOT STATUS EQUAL 0)
        message(FATAL_ERROR
            "legacy-goldens-biome ${ARM} FAILED (exit ${STATUS}):\n${ERR}\n"
            "Until the decoder recovers a seeding this repository already "
            "validates, nothing the scan says about the legacy derivation "
            "means anything. See SPEC §11.")
    endif()
    if(ARM STREQUAL "--control")
        set(CONTROL_OUT "${OUT}")
    endif()
endforeach()

# The control's own three figures, matched as text for the same reason
# --modern's are below: SPEC §11 prints a table of them, and a table nobody
# checks drifts. --control already fails on its own thresholds, but those are
# ranges — "recovers a correct seeding, puts a wrong one at the null" survives
# a positive arm sliding from 99.94% to 99.2%, and the SPEC table would then be
# quoting a number this build no longer produces.
foreach(FIGURE "32748/32768 = 99\\.9390%" "1858/32768 = 5\\.6702%"
               "7307/114688 = 6\\.3712%")
    if(NOT CONTROL_OUT MATCHES "${FIGURE}")
        message(FATAL_ERROR
            "the control no longer produces the figure SPEC §11 quotes "
            "(${FIGURE}). The three are: 32748/32768 = 99.9390% at worldSeed, "
            "1858/32768 = 5.6702% at worldSeed + 1, and a 7307/114688 = "
            "6.3712% vanilla-against-vanilla null. Update SPEC §11 and this "
            "check together, or find out what moved:\n${CONTROL_OUT}")
    endif()
endforeach()

# The number vanilla_legacy_nether_climate_gap_test.cpp measured with entirely
# different code. Matched as text so that a drift shows up here rather than in
# a summary someone writes from memory.
execute_process(
    COMMAND "${ANALYZER}" "${STRATUM_FIXTURES_DIR}" --modern
    OUTPUT_VARIABLE MODERN_OUT
)
if(NOT MODERN_OUT MATCHES "8873/32768 = 27\\.08%")
    message(FATAL_ERROR
        "the modern derivation no longer scores 8873/32768 = 27.08% on the "
        "nether goldens:\n${MODERN_OUT}")
endif()
if(NOT MODERN_OUT MATCHES "chance baseline 30\\.23%")
    message(FATAL_ERROR
        "the modern derivation's chance baseline is no longer 30.23%:\n${MODERN_OUT}")
endif()

# The two analyzers' candidate enumerations must index the same rule.
execute_process(
    COMMAND "${ANALYZER}" "${STRATUM_FIXTURES_DIR}" --candidate 182 0
    RESULT_VARIABLE STATUS
    OUTPUT_VARIABLE CANDIDATE_OUT
    ERROR_VARIABLE ERR
)
message(STATUS "${CANDIDATE_OUT}")
if(NOT STATUS EQUAL 0)
    message(FATAL_ERROR "--candidate 182 0 failed (exit ${STATUS}):\n${ERR}")
endif()
if(NOT CANDIDATE_OUT MATCHES "lcgLong xor md5FirstBE, forks 1, lcg")
    message(FATAL_ERROR
        "rule index 182 no longer names deepslate's own derivation, which "
        "tools/analysis/legacy-seed-analyze.cpp's header says it is. The two "
        "analyzers' candidate enumerations have drifted apart and no rule "
        "index carried between them means anything:\n${CANDIDATE_OUT}")
endif()

# The two arms added after a review found the header describing a `--split`
# that returned "unknown mode". Neither can be run in full here — --split is a
# second 270,000-candidate scan and --null-full all is an hour — so each is
# smoke-run through the same code the long form uses: one candidate scored with
# the shift held at zero (the Shift::Zero path --split takes for all of them),
# and a two-draw null at the full denominator. A documented mode that has
# stopped existing now fails a test rather than a reader.
execute_process(
    COMMAND "${ANALYZER}" "${STRATUM_FIXTURES_DIR}" --candidate 182 0 zero-shift
    RESULT_VARIABLE STATUS
    OUTPUT_VARIABLE SPLIT_OUT
    ERROR_VARIABLE ERR
)
message(STATUS "${SPLIT_OUT}")
if(NOT STATUS EQUAL 0 OR NOT SPLIT_OUT MATCHES "with the shift held at zero")
    message(FATAL_ERROR
        "--candidate ... zero-shift, the path --split scores the whole space "
        "with, no longer works (exit ${STATUS}):\n${SPLIT_OUT}${ERR}")
endif()

execute_process(
    COMMAND "${ANALYZER}" "${STRATUM_FIXTURES_DIR}" --null-full 2
    RESULT_VARIABLE STATUS
    OUTPUT_VARIABLE NULL_OUT
    ERROR_VARIABLE ERR
)
message(STATUS "${NULL_OUT}")
if(NOT STATUS EQUAL 0 OR NOT NULL_OUT MATCHES "null over 2 candidates at 24576 cells")
    message(FATAL_ERROR
        "--null-full, which measures the null at the denominator SPEC §11 "
        "quotes its best candidate at, no longer works (exit "
        "${STATUS}):\n${NULL_OUT}${ERR}")
endif()

message(STATUS "legacy-goldens-biome control passed")
