#!/usr/bin/env bash
# Runs the mcdoc generator's own tests, or skips visibly without python3.
# Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

PYTHON="${PYTHON:-python3}"
if ! command -v "${PYTHON}" >/dev/null 2>&1; then
    echo "no ${PYTHON} found; the mcdoc generator was NOT tested. Install" >&2
    echo "Python 3, or set PYTHON=/path/to/python3." >&2
    # 77 is what CTest reads as "skipped" (SKIP_RETURN_CODE, see
    # tests/CMakeLists.txt): a developer without the interpreter gets a
    # visible skip rather than a green run that checked nothing. tools/mcdoc-sync
    # needs the same interpreter, so in CI this is never taken.
    exit 77
fi

exec "${PYTHON}" "${here}/mcdoc_generator_test.py"
