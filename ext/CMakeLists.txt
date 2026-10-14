# ext/ — the PocketMine-MP binding (Milestone M5, SPEC 4.3 and 9).
#
# Split the way ext-nukkit/ is. stratum_pmmp_core is plain C++ — the chunk
# encoding PocketMine-MP loads sub-chunks from — with no PHP in it, so it is
# built and tested on every platform CI runs. The zend shim that wraps it
# needs PHP's headers and is its own target behind STRATUM_BUILD_EXT.

add_library(stratum_pmmp_core STATIC
    src/chunk_encoder.cpp
    src/dimension_registry.cpp
    src/fluid_updates.cpp
    src/worlds.cpp
)
add_library(stratum::pmmp_core ALIAS stratum_pmmp_core)

target_include_directories(stratum_pmmp_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# Threads: compiled dimensions, fluid-update outboxes and the main thread's
# pins are shared between PocketMine-MP's generation workers and its main
# thread (include/stratum_pmmp/worlds.hpp).
find_package(Threads REQUIRED)
target_link_libraries(stratum_pmmp_core PUBLIC
    stratum::core
    stratum::mapping
    Threads::Threads
)

stratum_configure_target(stratum_pmmp_core)

# --- The zend module ----------------------------------------------------------
#
# Built against a thread-safe PHP's php-config: PocketMine-MP runs ZTS PHP,
# and a module built for non-ZTS refuses to load into it. PocketMine-MP's own
# PHP releases ship no headers, so tools/php-dev builds a matching one from
# source (SPEC §11's M5-PMMP entry).
if(STRATUM_BUILD_EXT)
    set(STRATUM_PHP_CONFIG "" CACHE FILEPATH
        "php-config of the ZTS PHP to build ext/ against (tools/php-dev --print-config)")
    if(NOT STRATUM_PHP_CONFIG)
        find_program(STRATUM_PHP_CONFIG_ON_PATH NAMES php-config)
        set(STRATUM_PHP_CONFIG "${STRATUM_PHP_CONFIG_ON_PATH}")
    endif()
    if(NOT STRATUM_PHP_CONFIG OR NOT EXISTS "${STRATUM_PHP_CONFIG}")
        message(FATAL_ERROR
            "STRATUM_BUILD_EXT=ON needs a ZTS PHP's php-config. Build one with tools/php-dev "
            "and pass -DSTRATUM_PHP_CONFIG=$(tools/php-dev --print-config).")
    endif()

    foreach(query includes configure-options php-binary extension-dir prefix)
        string(REPLACE "-" "_" variable "php_${query}")
        execute_process(COMMAND "${STRATUM_PHP_CONFIG}" "--${query}"
            OUTPUT_VARIABLE ${variable} OUTPUT_STRIP_TRAILING_WHITESPACE
            COMMAND_ERROR_IS_FATAL ANY)
    endforeach()
    if(NOT php_configure_options MATCHES "--enable-zts")
        message(FATAL_ERROR
            "${STRATUM_PHP_CONFIG} is not a thread-safe (ZTS) PHP, and PocketMine-MP only loads "
            "modules built for one. Use tools/php-dev.")
    endif()
    separate_arguments(php_includes UNIX_COMMAND "${php_includes}")
    list(TRANSFORM php_includes REPLACE "^-I" "")

    add_library(stratum_php MODULE zend/php_stratum.cpp)
    set_target_properties(stratum_php PROPERTIES PREFIX "" OUTPUT_NAME stratum)
    # SYSTEM: PHP's own headers are not held to this project's warning set.
    target_include_directories(stratum_php SYSTEM PRIVATE ${php_includes})
    target_compile_definitions(stratum_php PRIVATE
        COMPILE_DL_STRATUM
        ZEND_ENABLE_STATIC_TSRMLS_CACHE=1
    )
    target_link_libraries(stratum_php PRIVATE stratum::pmmp_core)
    if(APPLE)
        # zend symbols resolve against the PHP binary that loads the module.
        target_link_options(stratum_php PRIVATE -undefined dynamic_lookup)
    endif()
    stratum_configure_target(stratum_php)

    if(STRATUM_BUILD_TESTS)
        # PHPT, through PHP's own runner, with the real chunkutils2 loaded so
        # the shim's output goes through the actual PalettedBlockArray::fromData.
        # Through tools/run-phpt, which fails on a SKIP: run-tests.php exits 0
        # when every test skips, and a suite that checked nothing must not
        # read as a pass.
        add_test(NAME ext.phpt
            COMMAND "${CMAKE_SOURCE_DIR}/tools/run-phpt"
                "${php_php_binary}" "${php_prefix}/lib/php/build/run-tests.php"
                --show-diff -n
                -d "extension=${php_extension_dir}/chunkutils2.so"
                -d "extension=$<TARGET_FILE:stratum_php>"
                --
                "${CMAKE_CURRENT_SOURCE_DIR}/zend/tests"
                "${CMAKE_CURRENT_SOURCE_DIR}/plugin/tests"
        )
        # The same fixture location the conformance suites read, so a
        # -DSTRATUM_FIXTURES_DIR (a git worktree pointing at the main
        # checkout's fixtures) reaches these too. Declared here with the
        # default tests/conformance/ gives it, because ext/ is configured
        # first; the second declaration there is then a no-op.
        set(STRATUM_FIXTURES_DIR "${CMAKE_SOURCE_DIR}/.fixtures" CACHE PATH
            "Location of locally generated, never-committed vanilla fixtures")
        set_tests_properties(ext.phpt PROPERTIES
            LABELS "ext"
            ENVIRONMENT "STRATUM_FIXTURES_DIR=${STRATUM_FIXTURES_DIR};NO_INTERACTION=1"
        )

        # Inside a real PocketMine-MP 5.44.3, on PocketMine-MP's own PHP
        # (tools/pmmp-dev; SPEC §11, "M5: Stratum inside a real
        # PocketMine-MP"). Local evidence, not CI: every case SKIPs, saying
        # what is missing, when tools/pmmp-dev has not fetched
        # ~/.cache/stratum-pmmp ($STRATUM_PMMP_DIR) or the fixtures are
        # absent. Run them with `ctest --preset pmmp`, which takes them one
        # at a time: the smokes run a server.
        set(pmmp_dev "${CMAKE_SOURCE_DIR}/tools/pmmp-dev")
        set(pmmp_root "${STRATUM_FIXTURES_DIR}/${STRATUM_MINECRAFT_VERSION}")
        set(pmmp_so --stratum-so "$<TARGET_FILE:stratum_php>")
        add_test(NAME pmmp.load
            COMMAND "${pmmp_dev}" check-load ${pmmp_so} --build-php-config "${STRATUM_PHP_CONFIG}")
        # Driven by the run-tests.php of the PHP the module was built
        # against (tools/php-dev's, from PHP's official source), never a
        # script out of the downloaded tarball.
        add_test(NAME pmmp.phpt
            COMMAND "${pmmp_dev}" phpt ${pmmp_so} --build-php-config "${STRATUM_PHP_CONFIG}"
                --fixtures-dir "${STRATUM_FIXTURES_DIR}" --
                "${CMAKE_CURRENT_SOURCE_DIR}/zend/tests" "${CMAKE_CURRENT_SOURCE_DIR}/plugin/tests")
        add_test(NAME pmmp.census.blocks
            COMMAND "${pmmp_dev}" census-blocks ${pmmp_so} --version-root "${pmmp_root}"
                --report "${CMAKE_CURRENT_BINARY_DIR}/pmmp-census-blocks.tsv")
        add_test(NAME pmmp.census.biomes
            COMMAND "${pmmp_dev}" census-biomes ${pmmp_so} --version-root "${pmmp_root}")
        add_test(NAME pmmp.threads
            COMMAND "${pmmp_dev}" threads ${pmmp_so} --version-root "${pmmp_root}")
        foreach(dimension overworld end)
            add_test(NAME pmmp.smoke.${dimension}
                COMMAND "${pmmp_dev}" smoke ${pmmp_so} --version-root "${pmmp_root}"
                    --dimension ${dimension} --work-dir "${CMAKE_CURRENT_BINARY_DIR}/pmmp-smoke-${dimension}")
        endforeach()
        # The overworld smoke observed from inside the server (the evidence
        # plugin, ext/pocketmine/plugin.yml): WorldLoadEvent before the first
        # chunk request, and the pinned world's fluid updates delivered after
        # PocketMine-MP shuts its idle async workers down. About seven
        # minutes, five of them the 300 s idle wait shutdownUnusedWorkers()
        # asks for. Reports PocketMine-MP's discarded population results;
        # pass --max-discards to fail above a count.
        add_test(NAME pmmp.observe
            COMMAND "${pmmp_dev}" observe ${pmmp_so} --version-root "${pmmp_root}"
                --work-dir "${CMAKE_CURRENT_BINARY_DIR}/pmmp-observe")
        set_tests_properties(pmmp.load pmmp.phpt pmmp.census.blocks pmmp.census.biomes pmmp.threads
            pmmp.smoke.overworld pmmp.smoke.end pmmp.observe PROPERTIES
            LABELS "pmmp"
            SKIP_RETURN_CODE 77
            RUN_SERIAL TRUE
        )
    endif()
endif()
