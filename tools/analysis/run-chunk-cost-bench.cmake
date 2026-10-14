# Drives stratum_chunk_cost_bench for its two CTest cases (tools/analysis/
# CMakeLists.txt): MODE=compare checks --compare on dump directories written
# here, MODE=fixtures runs the bench on the fetched worldgen tree.
#
# Every directory this writes is under WORK, in the build tree, and is
# replaced on each run. The fixtures mode's dumps are vanilla-derived (SPEC
# §12); the bench puts a `.gitignore` of `*` beside them, checked below.

# run(<expected exit> <what> <pattern or -> args...): runs the bench and
# fails naming <what> unless it exits <expected exit> with <pattern> in its
# output.
function(run expected what pattern)
    execute_process(
        COMMAND "${BENCH}" ${ARGN}
        RESULT_VARIABLE status
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err
    )
    if(NOT status STREQUAL "${expected}")
        message(FATAL_ERROR "${what}: exit ${status}, wanted ${expected}\n${out}\n${err}")
    endif()
    if(NOT pattern STREQUAL "-")
        string(FIND "${out}${err}" "${pattern}" at)
        if(at EQUAL -1)
            message(FATAL_ERROR "${what}: no '${pattern}' in its output\n${out}\n${err}")
        endif()
    endif()
    message(STATUS "ok: ${what}")
    set(LAST_OUTPUT "${out}" PARENT_SCOPE)
endfunction()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

if(MODE STREQUAL "compare")
    foreach(side a b prefix onlyone byte)
        file(MAKE_DIRECTORY "${WORK}/${side}")
    endforeach()
    file(MAKE_DIRECTORY "${WORK}/empty")
    foreach(side a b prefix onlyone byte)
        file(WRITE "${WORK}/${side}/end_seed0.dump" "the same bytes on every side")
    endforeach()
    foreach(side a b prefix byte)
        file(WRITE "${WORK}/${side}/overworld_seed0.dump" "0123456789")
    endforeach()
    file(WRITE "${WORK}/prefix/overworld_seed0.dump" "0123456789 and more")
    file(WRITE "${WORK}/byte/overworld_seed0.dump" "01234X6789")
    # Not a dump: --compare reads *.dump only, so the .gitignore the bench
    # writes, or anything else beside the dumps, is not compared.
    file(WRITE "${WORK}/b/.gitignore" "*\n")
    file(WRITE "${WORK}/b/notes.txt" "not a dump")

    run(0 "identical directories compare identical" "identical: 2 file(s), 38 bytes"
        --compare "${WORK}/a" "${WORK}/b")
    run(1 "a changed byte is found at its offset" "DIFFERENT overworld_seed0.dump: first at byte 5"
        --compare "${WORK}/a" "${WORK}/byte")
    run(1 "a file that only grew is a difference" "first at byte 10"
        --compare "${WORK}/a" "${WORK}/prefix")
    run(1 "a dump on one side only is a difference" "DIFFERENT overworld_seed0.dump: only in"
        --compare "${WORK}/a" "${WORK}/onlyone")
    run(1 "the difference count covers every file" "dumps differ: 1 of 2 file(s)"
        --compare "${WORK}/a" "${WORK}/byte")
    run(2 "two directories with no dumps are refused" "holds a *.dump file"
        --compare "${WORK}/empty" "${WORK}/empty")
    run(2 "a missing directory is refused" "is not a directory"
        --compare "${WORK}/a" "${WORK}/nowhere")
    run(2 "--compare takes exactly two directories" "takes two dump directories"
        --compare "${WORK}/a")
    run(2 "an unknown option is refused" "unknown option '--frobnicate'"
        "${WORK}" --frobnicate 1)
    run(2 "an unknown dimension is refused by name" "unknown dimension 'nether'"
        "${WORK}" --dimensions overworld,nether)
    return()
endif()

if(NOT MODE STREQUAL "fixtures")
    message(FATAL_ERROR "MODE must be compare or fixtures, not '${MODE}'")
endif()

set(VERSION_DIR "${STRATUM_FIXTURES_DIR}/${STRATUM_MINECRAFT_VERSION}")
if(NOT IS_DIRECTORY "${VERSION_DIR}/worldgen/noise_settings"
   OR NOT IS_DIRECTORY "${VERSION_DIR}/biome_parameters")
    message(STATUS
        "no worldgen or biome_parameters under '${VERSION_DIR}'. They are Mojang-derived "
        "and never committed (SPEC §12); tools/fetch-vanilla writes them.")
    message(STATUS "SKIP: no fetched worldgen tree")
    # cmake_language(EXIT) exists only from CMake 3.29 (run-legacy-seed-
    # control.cmake says why the bare call cannot be used below it).
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
        cmake_language(EXIT 77)
    endif()
    return()
endif()

set(ONE_CHUNK --seeds 0 --scattered off --warmup 0 --repeat 1)
run(0 "the bench runs one overworld and one End chunk" "end, every seed, 1 chunk(s) each"
    "${VERSION_DIR}" --dimensions overworld,end ${ONE_CHUNK} --spiral 0,0,0
    --dump "${WORK}/a" --json "${WORK}/a.json")
run(0 "and runs them again" "load average at end"
    "${VERSION_DIR}" --dimensions overworld,end ${ONE_CHUNK} --spiral 0,0,0
    --dump "${WORK}/b")
run(0 "two runs of the same chunks write identical dumps" "identical: 2 file(s)"
    --compare "${WORK}/a" "${WORK}/b")
run(0 "another End chunk" "-"
    "${VERSION_DIR}" --dimensions end ${ONE_CHUNK} --spiral -3,2,0 --dump "${WORK}/c")
run(1 "another chunk's dump differs, inside its chunk record" "in chunk (0, 0), record 0"
    --compare "${WORK}/a" "${WORK}/c")
string(FIND "${LAST_OUTPUT}" "DIFFERENT overworld_seed0.dump: only in" at)
if(at EQUAL -1)
    message(FATAL_ERROR "a dump on one side only was not reported:\n${LAST_OUTPUT}")
endif()

file(READ "${WORK}/a/.gitignore" ignore)
if(NOT ignore MATCHES "\n\\*\n")
    message(FATAL_ERROR "the dump directory's .gitignore does not ignore everything:\n${ignore}")
endif()

file(READ "${WORK}/a.json" json)
foreach(dimension overworld end)
    foreach(stage fill biomes encode sum e2e)
        string(JSON available GET "${json}" dimensions ${dimension} all ${stage} wall available)
        string(JSON chunks GET "${json}" dimensions ${dimension} all ${stage} wall chunks)
        if(NOT available OR NOT chunks EQUAL 1)
            message(FATAL_ERROR "${dimension} ${stage}: available ${available}, ${chunks} chunk(s)")
        endif()
    endforeach()
    string(JSON hash GET "${json}" dimensions ${dimension} seeds 0 dump_fnv1a)
    string(LENGTH "${hash}" length)
    if(NOT length EQUAL 16)
        message(FATAL_ERROR "${dimension}: no dump hash in the JSON")
    endif()
endforeach()
foreach(field build_config optimised cpu governor engine_version)
    string(JSON value GET "${json}" machine ${field})
    message(STATUS "machine ${field}: ${value}")
endforeach()
message(STATUS "the chunk-cost bench: staged encode matches encodeChunk, dumps are stable")
