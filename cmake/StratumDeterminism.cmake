# Stratum — determinism flags (SPEC section 5, CLAUDE.md "Determinism rules").
#
# Same (pipeline, seed, chunk) must produce identical bytes on every
# supported compiler and architecture. Floating point is therefore pinned:
#   * no -ffast-math / -funsafe-math-optimizations, ever;
#   * no FMA contraction, so x86-64 and ARM64 agree bit-for-bit;
#   * no x87 excess precision on 32-bit x86.
# These are release blockers, not preferences. Do not relax them for speed.

set(_STRATUM_BANNED_FLAGS
    "-ffast-math"
    "-funsafe-math-optimizations"
    "-fassociative-math"
    "-freciprocal-math"
    "-ffinite-math-only"
    "/fp:fast"
)

# Fail configuration if a banned flag reaches us via the cache or environment.
function(stratum_assert_no_banned_flags)
    set(_scanned
        "${CMAKE_CXX_FLAGS}"
        "${CMAKE_CXX_FLAGS_DEBUG}"
        "${CMAKE_CXX_FLAGS_RELEASE}"
        "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}"
        "${CMAKE_C_FLAGS}"
        "$ENV{CXXFLAGS}"
    )
    foreach(_flags IN LISTS _scanned)
        foreach(_banned IN LISTS _STRATUM_BANNED_FLAGS)
            string(FIND "${_flags}" "${_banned}" _hit)
            if(NOT _hit EQUAL -1)
                message(FATAL_ERROR
                    "Determinism violation (SPEC 5): banned flag '${_banned}' "
                    "found in compiler flags '${_flags}'. Fast-math style "
                    "optimizations change worldgen output and are forbidden in "
                    "every build configuration.")
            endif()
        endforeach()
    endforeach()
endfunction()

function(stratum_set_determinism target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /fp:precise)
    else()
        target_compile_options(${target} PRIVATE
            -ffp-contract=off
            -fno-fast-math
        )
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options(${target} PRIVATE -fexcess-precision=standard)
        endif()
        # 32-bit x86 defaults to the x87 stack, whose 80-bit intermediates
        # diverge from every other target. Force SSE2 arithmetic there.
        if(CMAKE_SIZEOF_VOID_P EQUAL 4
           AND CMAKE_SYSTEM_PROCESSOR MATCHES "(i[3-6]86|x86)")
            target_compile_options(${target} PRIVATE -msse2 -mfpmath=sse)
        endif()
    endif()
endfunction()

# Every first-party target goes through this.
function(stratum_configure_target target)
    stratum_set_warnings(${target})
    stratum_set_determinism(${target})
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        INTERPROCEDURAL_OPTIMIZATION OFF
    )
endfunction()
