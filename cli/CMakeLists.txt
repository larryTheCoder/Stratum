# cli/ — standalone tools built on lib/. No PocketMine, no PHP.
#
# Subcommands (SPEC 4): render, generate, diff, validate. They are the
# vehicle for the conformance harness (SPEC 7): `stratum diff` is what
# compares engine output against vanilla region files in Java block space.

# Threads: `stratum generate --threads N` generates a region's chunks on N
# of them.
find_package(Threads REQUIRED)

add_executable(stratum_cli
    src/main.cpp
    src/generate.cpp
)
add_executable(stratum::cli ALIAS stratum_cli)

set_target_properties(stratum_cli PROPERTIES OUTPUT_NAME "stratum")

target_link_libraries(stratum_cli PRIVATE stratum::core Threads::Threads)

stratum_configure_target(stratum_cli)
