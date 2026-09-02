# Stratum — JSON provisioning.
#
# SPEC §11 picks nlohmann/json: parsing happens once at world load, so
# ergonomics beat throughput. Provisioned the same way as the other pinned
# dependencies — a hash-verified release archive, never a git clone, so the
# source tree cannot change under a build.

option(STRATUM_FETCH_JSON "Fetch nlohmann/json when the system has none" ON)
set(STRATUM_JSON_TAG "v3.12.0" CACHE STRING "Pinned nlohmann/json release tag")
set(STRATUM_JSON_SHA256
    "4b92eb0c06d10683f7447ce9406cb97cd4b453be18d7279320f7b2f025c10187"
    CACHE STRING "SHA-256 of the pinned nlohmann/json release archive")

function(stratum_provision_json)
    if(TARGET nlohmann_json::nlohmann_json)
        return()
    endif()

    find_package(nlohmann_json 3.11 QUIET GLOBAL)
    if(nlohmann_json_FOUND)
        message(STATUS "nlohmann/json: system ${nlohmann_json_VERSION}")
        return()
    endif()

    if(NOT STRATUM_FETCH_JSON)
        message(FATAL_ERROR
            "nlohmann/json was not found and STRATUM_FETCH_JSON=OFF. Worldgen "
            "definitions are JSON, so loading a pack needs it. Install "
            "nlohmann-json3-dev or configure with -DSTRATUM_FETCH_JSON=ON.")
    endif()

    message(STATUS "nlohmann/json: not found on the system, fetching ${STRATUM_JSON_TAG}")
    include(FetchContent)
    set(JSON_BuildTests OFF CACHE INTERNAL "")
    set(JSON_Install OFF CACHE INTERNAL "")
    FetchContent_Declare(nlohmann_json
        URL "https://github.com/nlohmann/json/archive/refs/tags/${STRATUM_JSON_TAG}.tar.gz"
        URL_HASH SHA256=${STRATUM_JSON_SHA256}
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(nlohmann_json)
endfunction()
