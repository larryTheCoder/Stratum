# Stratum — project warning set.
#
# CLAUDE.md ("Definition of done") requires -Wall -Wextra with no new
# warnings, and warnings-as-errors in CI. The extra conversion/promotion
# warnings are deliberate: this codebase reimplements Java integer and
# float semantics, where an implicit narrowing or an accidental float ->
# double promotion is a parity bug, not a style issue.

function(stratum_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /permissive-
            /Zc:__cplusplus
            /Zc:preprocessor
            /w14242  # conversion, possible loss of data
            /w14254  # larger bit-field conversion
            /w14263  # member function does not override
            /w14265  # non-virtual destructor
            /w14287  # unsigned/negative constant mismatch
            /w14296  # expression is always true/false
            /w14311  # pointer truncation
            /w14826  # sign-extended conversion
            /w14905  # wide string literal cast
            /w14906  # string literal cast
            /w14928  # illegal copy-initialization
        )
        if(STRATUM_WERROR)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wshadow
            -Wconversion
            -Wsign-conversion
            -Wdouble-promotion
            -Wfloat-equal
            -Wold-style-cast
            -Wcast-qual
            -Wnon-virtual-dtor
            -Woverloaded-virtual
            -Wnull-dereference
            -Wformat=2
            -Wimplicit-fallthrough
        )
        if(STRATUM_WERROR)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()
