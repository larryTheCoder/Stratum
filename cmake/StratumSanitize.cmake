# Stratum — the undefined-behaviour sanitizer build (SPEC §5.7, the
# `sanitize` preset).
#
# SPEC §5 asks for identical output on every platform, and undefined
# behaviour is the one thing that cannot meet it: an out-of-range
# float-to-int conversion is INT32_MIN on x86-64 and saturates on ARM64, so
# the same chunk differs between the two legs, or the optimiser removes the
# code outright. So no input may reach it: §8 refuses, by name, every input
# that would. This build is how that is held — every first-party target under
# UBSan, the first report fatal.
#
# The checks are `undefined` plus `float-cast-overflow`, spelled out (it is
# part of `undefined` in current Clang, and the conversion this build exists
# for above all). NOT enabled, on purpose, because what they report is
# DEFINED behaviour this engine relies on:
#   * float-divide-by-zero — IEEE division by zero is an infinity, and
#     `minecraft:invert` of a zero argument is one;
#   * unsigned-integer-overflow — every seed and hash is wrapping uint64
#     arithmetic by design (SPEC §5.2);
#   * implicit-conversion — narrowing to a smaller unsigned type is modular
#     by definition, and the lattice indices are exactly that.
#
# `_GLIBCXX_ASSERTIONS` turns libstdc++'s preconditions into checks too:
# `std::clamp` with its bounds reversed is undefined without it, and was
# reachable. It changes no layout, so third-party code built without it links
# unchanged.
#
# None of this touches floating point: the determinism flags are applied to
# every target exactly as in any other build (StratumDeterminism.cmake), and
# the sanitizer adds checks around operations, not different operations.

option(STRATUM_SANITIZE
    "Build first-party targets under UBSan (undefined + float-cast-overflow, no recovery)" OFF)

set(_STRATUM_SANITIZE_FLAGS
    "-fsanitize=undefined,float-cast-overflow"
    "-fno-sanitize-recover=all"
    "-fno-omit-frame-pointer"
)

# Called once, at configure: refuses a toolchain that cannot honour the flags
# rather than building without them.
function(stratum_check_sanitize_toolchain)
    if(NOT STRATUM_SANITIZE)
        return()
    endif()
    if(MSVC OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        message(FATAL_ERROR
            "STRATUM_SANITIZE needs Clang or GCC (UBSan with float-cast-overflow); "
            "${CMAKE_CXX_COMPILER_ID} has no equivalent. Use the `sanitize` preset, which "
            "selects clang++.")
    endif()
endfunction()

# Every first-party target goes through this, from stratum_configure_target.
function(stratum_set_sanitizers target)
    if(NOT STRATUM_SANITIZE)
        return()
    endif()
    target_compile_options(${target} PRIVATE ${_STRATUM_SANITIZE_FLAGS})
    target_compile_definitions(${target} PRIVATE _GLIBCXX_ASSERTIONS)
    # Only what links needs the runtime; a static library is archived, and
    # the executable that links it brings the runtime in.
    get_target_property(_type ${target} TYPE)
    if(_type STREQUAL "EXECUTABLE" OR _type STREQUAL "SHARED_LIBRARY"
       OR _type STREQUAL "MODULE_LIBRARY")
        target_link_options(${target} PRIVATE ${_STRATUM_SANITIZE_FLAGS})
    endif()
endfunction()
