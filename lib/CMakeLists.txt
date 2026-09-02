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
    src/chunk.cpp
    src/density_graph.cpp
    src/density_interpreter.cpp
    src/density_render.cpp
    src/fdlibm_log.cpp
    src/md5.cpp
    src/pack.cpp
    src/pack_validate.cpp
    src/nbt_reader.cpp
    src/noise_registry.cpp
    src/perlin.cpp
    src/registry.cpp
    src/resource_location.cpp
    src/png.cpp
    src/nbt_tag.cpp
    src/region_diff.cpp
    src/region_render.cpp
    src/region_file.cpp
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
        "${CMAKE_CURRENT_SOURCE_DIR}/include/stratum/chunk/chunk.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/stratum/data/pack.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/stratum/density/graph.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/stratum/density/interpreter.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/stratum/density/noise_registry.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/stratum/data/registry.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/stratum/data/resource_location.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/stratum/javamath.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/stratum/hash/md5.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/stratum/image/png.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/stratum/math/fdlibm.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/stratum/nbt/reader.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/stratum/nbt/tag.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/stratum/noise/perlin.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/stratum/region/diff.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/stratum/region/region_file.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/stratum/render/density_render.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/stratum/render/region_render.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/stratum/rng/java_random.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/stratum/rng/xoroshiro128.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/stratum/validate/pack_report.hpp"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/stratum/version.hpp"
)

target_link_libraries(stratum_core
    PUBLIC nlohmann_json::nlohmann_json
    PRIVATE ZLIB::ZLIB
)

stratum_configure_target(stratum_core)

# lib/mapping/ (Java -> Bedrock state and biome mapping) is Milestone M5 and
# has no sources yet; it will be added here as its own target with its own
# tests, deliberately downstream of the conformance boundary (SPEC 9).
