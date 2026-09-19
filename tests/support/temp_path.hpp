// Stratum — process-unique scratch paths for tests.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// catch_discover_tests gives every Catch2 case its own process, so `ctest -j`
// runs several of them concurrently. A scratch name that is only unique
// *within* a process is therefore not unique at all: every concurrent process
// hands out the same first name, and a fixture whose constructor clears its
// directory (remove_all, as most of ours do) deletes another process's tree
// out from under a running test. That is the whole of the parallel flakiness
// this header exists to remove — it is not a tidying convenience.
//
// Uniqueness comes from the process id plus a per-process counter. The pid
// separates concurrent processes; the counter separates fixtures inside one.
// Each call site keeps its own descriptive stem, so a file left behind by a
// crash still says which test dropped it.
//
// Residual, stated as a bound rather than a guarantee: the operating system
// may reuse a pid once the process holding it has exited. Reuse is therefore
// only reachable after the earlier process has finished and run its cleanup,
// and every site here removes what it created. A crashed process that leaked
// a tree and a later process that draws the same pid would still collide —
// the constructors' remove_all is what covers that case, as it did before.

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace stratum::test {

/// The current process id. `_getpid` is the spelling MSVC does not deprecate;
/// the bare POSIX `getpid` is the one that trips its CRT warnings.
[[nodiscard]] inline std::uint64_t currentProcessId() {
#ifdef _WIN32
    return static_cast<std::uint64_t>(::_getpid());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

/// A scratch file or directory name unique across every process in a `ctest
/// -j` run: "<stem>-<pid>-<serial><suffix>".
///
/// `suffix` carries an extension when one is wanted, so that it lands after
/// the serial ("stratum-cli-render-4021-3.png") rather than in the middle of
/// the name.
[[nodiscard]] inline std::string tempName(std::string_view stem, std::string_view suffix = {}) {
    static std::atomic<std::uint64_t> counter{0};
    const std::uint64_t serial = counter.fetch_add(1, std::memory_order_relaxed) + 1U;

    std::string name(stem);
    name += '-';
    name += std::to_string(currentProcessId());
    name += '-';
    name += std::to_string(serial);
    name += suffix;
    return name;
}

/// `tempName` under the system temporary directory.
[[nodiscard]] inline std::filesystem::path tempPath(std::string_view stem,
                                                    std::string_view suffix = {}) {
    return std::filesystem::temp_directory_path() / tempName(stem, suffix);
}

} // namespace stratum::test
