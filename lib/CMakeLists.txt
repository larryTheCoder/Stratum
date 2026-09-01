# lib/ — core static library.
#
# HARD RULE (SPEC 4, CLAUDE.md): this target never includes PHP or zend
# headers and never links against libphp. Anything that needs the PHP ABI
# belongs in ext/. Keeping this true is what makes cli/ and the conformance
# harness able to run the real engine without PocketMine.

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/stratum/version.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/stratum/version.hpp"
    @ONLY
)

add_library(stratum_core STATIC
    src/version.cpp
)
add_library(stratum::core ALIAS stratum_core)

target_include_directories(stratum_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

target_sources(stratum_core PUBLIC
    FILE_SET HEADERS
    BASE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_BINARY_DIR}/generated"
    FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/include/stratum/javamath.hpp"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/stratum/version.hpp"
)

stratum_configure_target(stratum_core)

# lib/mapping/ (Java -> Bedrock state and biome mapping) is Milestone M5 and
# has no sources yet; it will be added here as its own target with its own
# tests, deliberately downstream of the conformance boundary (SPEC 9).
