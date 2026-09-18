# Stratum — pipeline worldgen engine for PocketMine-MP.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#
# Layer separation is enforced here (SPEC 4):
#   lib/         pure C++, never links or includes PHP/zend
#   cli/         standalone tools built on lib/
#   ext/         thin zend binding for PocketMine-MP (Milestone M5)
#   ext-nukkit/  thin JNI binding for CloudburstMC/Nukkit (Milestone M5)
#   tests/       Catch2 v3 unit tests + conformance harness driver

cmake_minimum_required(VERSION 3.24)

project(stratum
    VERSION 0.1.0
    DESCRIPTION "Vanilla-schema pipeline worldgen engine for PocketMine-MP"
    HOMEPAGE_URL "https://github.com/larryTheCoder/stratum"
    LANGUAGES CXX
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# --------------------------------------------------------------------------
# Schema pin (SPEC 3). Changing these is deliberate migration work: schema
# diff review, regenerated conformance goldens, capability matrix re-check.
# --------------------------------------------------------------------------
set(STRATUM_MINECRAFT_VERSION "1.21.11" CACHE STRING
    "Pinned Java Edition version whose worldgen schema this engine executes")
set(STRATUM_PACK_FORMAT_MAJOR 94 CACHE STRING "Pinned data pack format major")
set(STRATUM_PACK_FORMAT_MINOR 1  CACHE STRING "Pinned data pack format minor")

# Bumped whenever a change can alter generated output for an existing stored
# pipeline (SPEC 6). Worlds record this; a mismatch the engine cannot
# reproduce is a hard refusal, never best-effort generation.
set(STRATUM_PIPELINE_ENGINE_VERSION 1 CACHE STRING
    "Pipeline execution engine version stamped into per-world freeze blobs")

# --------------------------------------------------------------------------
# Options
# --------------------------------------------------------------------------
option(STRATUM_WERROR      "Treat compiler warnings as errors (on in CI)" OFF)
option(STRATUM_BUILD_CLI   "Build the standalone cli/ tools"             ON)
option(STRATUM_BUILD_TESTS "Build the Catch2 test suites"                ON)
option(STRATUM_BUILD_EXT   "Build the PocketMine-MP zend binding (M5)"   OFF)
option(STRATUM_BUILD_EXT_NUKKIT "Build the CloudburstMC/Nukkit JNI binding (M5)" OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# ext-nukkit/'s stratum_nukkit is a shared library (a JNI target has to be);
# everything it statically links — stratum_core included, whether or not
# STRATUM_BUILD_EXT_NUKKIT is even on for this configure — needs -fPIC to
# be linkable into one. Global rather than per-target: stratum_core is
# built long before ext-nukkit/ is added, so a property set only there
# would need every earlier target rebuilt with a different flag anyway.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

include(StratumWarnings)
include(StratumDeterminism)
include(StratumJson)
include(StratumZlib)

stratum_assert_no_banned_flags()

# Provisioned here rather than in lib/: an imported target created inside a
# subdirectory is not visible to sibling directories, and tests/ links zlib
# too in order to build the region fixtures it reads back.
stratum_provision_zlib()
stratum_provision_json()

# --------------------------------------------------------------------------
# Layers
# --------------------------------------------------------------------------
# Before any layer: ext/ registers tests of its own, and add_test() is
# ignored in a directory processed before testing is enabled.
if(STRATUM_BUILD_TESTS)
    include(CTest)
endif()

add_subdirectory(lib)

if(STRATUM_BUILD_CLI)
    add_subdirectory(cli)
endif()

# ext/'s plain C++ half has no dependency beyond lib/, so it always builds
# and every platform tests it; its zend module needs a ZTS PHP and only
# builds with STRATUM_BUILD_EXT (ext/CMakeLists.txt refuses without one).
add_subdirectory(ext)

if(STRATUM_BUILD_EXT_NUKKIT)
    add_subdirectory(ext-nukkit)
endif()

if(STRATUM_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# After tests/, which is where STRATUM_FIXTURES_DIR is defined: the analysis
# tools' control test reads the probe worlds under it. They are here at all so
# that the build, the warning set, clang-format and clang-tidy reach them --
# see tools/analysis/CMakeLists.txt.
add_subdirectory(tools/analysis)

message(STATUS "Stratum ${PROJECT_VERSION} | engine v${STRATUM_PIPELINE_ENGINE_VERSION} "
               "| schema pin MC ${STRATUM_MINECRAFT_VERSION} "
               "pack format ${STRATUM_PACK_FORMAT_MAJOR}.${STRATUM_PACK_FORMAT_MINOR}")
