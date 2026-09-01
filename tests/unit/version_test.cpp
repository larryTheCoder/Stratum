// Stratum — version and schema-pin metadata tests.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0

#include <stratum/version.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_CASE("schema pin matches SPEC §3", "[version][pin]") {
    // The pin is a deliberate, versioned decision (SPEC §3): moving it means
    // schema diff review, regenerated conformance goldens and a capability
    // matrix re-check. This test exists so it cannot drift by accident.
    STATIC_REQUIRE(stratum::kMinecraftVersion == "1.21.11");
    STATIC_REQUIRE(stratum::kPackFormatMajor == 94);
    STATIC_REQUIRE(stratum::kPackFormatMinor == 1);
}

TEST_CASE("pack format major gate accepts only the pinned line", "[version][pin]") {
    STATIC_REQUIRE(stratum::isPinnedPackFormatMajor(94));
    STATIC_REQUIRE_FALSE(stratum::isPinnedPackFormatMajor(93));
    STATIC_REQUIRE_FALSE(stratum::isPinnedPackFormatMajor(95));
    STATIC_REQUIRE_FALSE(stratum::isPinnedPackFormatMajor(0));
    STATIC_REQUIRE_FALSE(stratum::isPinnedPackFormatMajor(-94));
}

TEST_CASE("pipeline engine version is stamped and positive", "[version][freeze]") {
    // Every per-world freeze blob records this (SPEC §6); zero would make a
    // stored pipeline indistinguishable from an unstamped one.
    STATIC_REQUIRE(stratum::kPipelineEngineVersion >= 1U);
}

TEST_CASE("version banner reports engine version and pin", "[version]") {
    const std::string banner = stratum::versionBanner();

    REQUIRE(banner.find("stratum ") == 0U);
    REQUIRE(banner.find(std::string(stratum::kVersion)) != std::string::npos);
    REQUIRE(banner.find("pipeline engine v1") != std::string::npos);
    REQUIRE(banner.find("schema pin MC 1.21.11") != std::string::npos);
    REQUIRE(banner.find("pack format 94.1") != std::string::npos);
}
