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
# produces it.
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

message(STATUS "legacy-goldens-biome control passed")
