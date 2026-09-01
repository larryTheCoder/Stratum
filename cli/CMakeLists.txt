# cli/ — standalone tools built on lib/. No PocketMine, no PHP.
#
# Subcommands (SPEC 4): render, generate, diff, validate. They are the
# vehicle for the conformance harness (SPEC 7): `stratum diff` is what
# compares engine output against vanilla region files in Java block space.

add_executable(stratum_cli
    src/main.cpp
)
add_executable(stratum::cli ALIAS stratum_cli)

set_target_properties(stratum_cli PROPERTIES OUTPUT_NAME "stratum")

target_link_libraries(stratum_cli PRIVATE stratum::core)

stratum_configure_target(stratum_cli)
