# Stratum — zlib provisioning.
#
# Region files store chunks deflated (zlib or gzip framing), so reading a
# .mca means inflating. A system zlib is used when there is one; otherwise
# it is fetched and built, because the CI matrix includes Windows runners
# that ship no system zlib and a missing dependency must not quietly turn
# into a missing feature.

option(STRATUM_FETCH_ZLIB "Fetch and build zlib when the system has none" ON)
set(STRATUM_ZLIB_TAG "v1.3.1" CACHE STRING "Pinned zlib release tag")
set(STRATUM_ZLIB_SHA256
    "17e88863f3600672ab49182f217281b6fc4d3c762bde361935e436a95214d05c"
    CACHE STRING "SHA-256 of the pinned zlib release archive")

function(stratum_provision_zlib)
    if(TARGET ZLIB::ZLIB)
        return()
    endif()

    # GLOBAL: the imported target must outlive this function's scope.
    find_package(ZLIB 1.2 QUIET GLOBAL)
    if(ZLIB_FOUND)
        message(STATUS "zlib: system ${ZLIB_VERSION_STRING}")
        return()
    endif()

    if(NOT STRATUM_FETCH_ZLIB)
        message(FATAL_ERROR
            "zlib was not found and STRATUM_FETCH_ZLIB=OFF. Region files are "
            "deflated, so reading them needs zlib. Install a zlib development "
            "package or configure with -DSTRATUM_FETCH_ZLIB=ON.")
    endif()

    message(STATUS "zlib: not found on the system, fetching ${STRATUM_ZLIB_TAG}")
    include(FetchContent)
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    # Verified archive rather than a clone: see the note on Catch2 in
    # tests/CMakeLists.txt.
    FetchContent_Declare(zlib
        URL "https://github.com/madler/zlib/archive/refs/tags/${STRATUM_ZLIB_TAG}.tar.gz"
        URL_HASH SHA256=${STRATUM_ZLIB_SHA256}
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(zlib)

    # Upstream's CMake exports `zlib` and `zlibstatic`, but no namespaced
    # target, so first-party code can depend on one spelling either way.
    if(NOT TARGET ZLIB::ZLIB)
        if(TARGET zlibstatic)
            add_library(ZLIB::ZLIB ALIAS zlibstatic)
        elseif(TARGET zlib)
            add_library(ZLIB::ZLIB ALIAS zlib)
        else()
            message(FATAL_ERROR
                "zlib was fetched but exported neither zlibstatic nor zlib; "
                "the pinned tag ${STRATUM_ZLIB_TAG} may have changed layout.")
        endif()
    endif()
endfunction()
