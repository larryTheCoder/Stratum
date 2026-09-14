# ext/ — the PocketMine-MP binding (Milestone M5, SPEC 4.3 and 9).
#
# Split the way ext-nukkit/ is. stratum_pmmp_core is plain C++ — the chunk
# encoding PocketMine-MP loads sub-chunks from — with no PHP in it, so it is
# built and tested on every platform CI runs. The zend shim that wraps it
# needs PHP's headers and is its own target behind STRATUM_BUILD_EXT.

add_library(stratum_pmmp_core STATIC
    src/chunk_encoder.cpp
)
add_library(stratum::pmmp_core ALIAS stratum_pmmp_core)

target_include_directories(stratum_pmmp_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(stratum_pmmp_core PUBLIC
    stratum::core
    stratum::mapping
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
        add_test(NAME ext.phpt
            COMMAND "${php_php_binary}" -n "${php_prefix}/lib/php/build/run-tests.php"
                -q --show-diff -p "${php_php_binary}" -n
                -d "extension=${php_extension_dir}/chunkutils2.so"
                -d "extension=$<TARGET_FILE:stratum_php>"
                "${CMAKE_CURRENT_SOURCE_DIR}/zend/tests"
        )
        set_tests_properties(ext.phpt PROPERTIES
            LABELS "ext"
            ENVIRONMENT "STRATUM_FIXTURES_DIR=${CMAKE_SOURCE_DIR}/.fixtures;NO_INTERACTION=1"
        )
    endif()
endif()
