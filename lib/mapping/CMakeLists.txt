# lib/mapping/ — Java -> Bedrock mapping layer (Milestone M5, SPEC section 9).
#
# Its own target, deliberately: this is downstream of the conformance
# boundary and must never be a dependency generation itself takes.
# stratum_core does not link this, and never should.

add_library(stratum_mapping STATIC
    src/biome_mapping.cpp
)

add_library(stratum::mapping ALIAS stratum_mapping)

target_include_directories(stratum_mapping PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_include_directories(stratum_mapping PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(stratum_mapping PUBLIC
    stratum::core
)

stratum_configure_target(stratum_mapping)
