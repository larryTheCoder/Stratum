// Stratum — version metadata implementation.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0

#include <stratum/version.hpp>

#include <string>

namespace stratum {

std::string versionBanner() {
    std::string banner;
    banner += "stratum ";
    banner += kVersion;
    banner += " (pipeline engine v";
    banner += std::to_string(kPipelineEngineVersion);
    banner += ", schema pin MC ";
    banner += kMinecraftVersion;
    banner += ", pack format ";
    banner += std::to_string(kPackFormatMajor);
    banner += '.';
    banner += std::to_string(kPackFormatMinor);
    banner += ')';
    return banner;
}

} // namespace stratum
