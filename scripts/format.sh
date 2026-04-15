#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ivi-homescreen contributors
#
# format.sh — apply clang-format-19 in-place to all C++ source files.
#
# Usage:
#   scripts/format.sh [--check]
#
# Without arguments: formats all *.cc / *.cpp / *.h / *.hpp files under
# shell/ and test/ using the project's .clang-format configuration.
#
# With --check: runs in dry-run mode and exits non-zero if any file would be
# reformatted.  This is the mode called by the CI lint workflow.

set -euo pipefail

CLANG_FORMAT="${CLANG_FORMAT:-clang-format-19}"

if ! command -v "${CLANG_FORMAT}" &>/dev/null; then
    echo "error: ${CLANG_FORMAT} not found. Install it (e.g. sudo apt install clang-format-19)." >&2
    exit 1
fi

# Resolve the repository root relative to this script so it works from any CWD.
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

mapfile -d '' FILES < <(
    find \
        "${REPO_ROOT}/shell" \
        "${REPO_ROOT}/test" \
        \( -name '*.cc' -o -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) \
        -not -path "${REPO_ROOT}/shell/platform/homescreen/client_wrapper/*" \
        -not -path "${REPO_ROOT}/shell/platform/homescreen/public/*" \
        -print0
)

if [[ ${#FILES[@]} -eq 0 ]]; then
    echo "No C++ source files found."
    exit 0
fi

if [[ "${1:-}" == "--check" ]]; then
    echo "Checking formatting with ${CLANG_FORMAT} (dry-run) ..."
    "${CLANG_FORMAT}" --dry-run -Werror "${FILES[@]}"
    echo "All files are correctly formatted."
else
    echo "Formatting ${#FILES[@]} file(s) with ${CLANG_FORMAT} ..."
    "${CLANG_FORMAT}" -i "${FILES[@]}"
    echo "Done."
fi