// Stratum — the zend module PocketMine-MP loads.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

extern "C" {
#include <php.h>

extern zend_module_entry stratum_module_entry; // NOLINT(readability-identifier-naming)
}

#define PHP_STRATUM_VERSION "0.1.0" // NOLINT(cppcoreguidelines-macro-usage)

#if defined(ZTS) && defined(COMPILE_DL_STRATUM)
extern "C" {
ZEND_TSRMLS_CACHE_EXTERN()
}
#endif
