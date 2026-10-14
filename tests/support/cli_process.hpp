// Stratum — running the real `stratum` binary from a test.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// The CLI's contract — which exit code means what, and what lands on disk —
// is what scripts and CI depend on, so the cases that hold it run the binary
// rather than the functions behind it. The unit suite does (cli_test.cpp),
// and so does the conformance case that scores `stratum generate`'s regions
// against the goldens (golden_generate_test.cpp). Each target passes the
// binary's path in as STRATUM_CLI_PATH.
#pragma once

#include "support/file_bytes.hpp"
#include "support/temp_path.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>

#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace stratum::test {

struct CliResult {
    int exitCode = 0;
    /// stdout and stderr together, in the order they were written.
    std::string output;
};

/// The exit code in what std::system returned. A by-value parameter rather
/// than read off a const local: Apple's WIFEXITED and WEXITSTATUS expand to
/// *(int *)&(x), which drops the qualifier off a const int and trips
/// -Wcast-qual.
[[nodiscard]] inline int exitCodeOf(int status) {
#ifdef _WIN32
    return status;
#else
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

/// Runs @p binary with @p arguments (already quoted as a shell needs them).
[[nodiscard]] inline CliResult runCli(const std::string& binary, const std::string& arguments) {
    const std::filesystem::path outputPath = tempPath("stratum-cli-test-output", ".txt");

#ifdef _WIN32
    // std::system runs `cmd /c <command>`, and cmd strips the outer quotes
    // when a command line both begins with a quote and contains more of
    // them, so the executable is never found. Wrapping the whole thing in
    // one more pair is the documented way round that.
    constexpr const char* kWrap = "\"";
#else
    constexpr const char* kWrap = "";
#endif
    const std::string command = std::string(kWrap) + "\"" + binary + "\" " + arguments + " > \"" +
                                outputPath.string() + "\" 2>&1" + kWrap;

    const int status = std::system(command.c_str());

    const std::string output = readFileBytes(outputPath).value_or(std::string());
    std::filesystem::remove(outputPath);

    return CliResult{exitCodeOf(status), output};
}

} // namespace stratum::test
