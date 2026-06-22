#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ivi-homescreen contributors
#
# clang_tidy.sh — run clang-tidy-19 across the project sources.
#
# Usage:
#   scripts/clang_tidy.sh [BUILD_DIR]
#
# BUILD_DIR defaults to "build" and must contain a compile_commands.json
# (configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR="${1:-build}"
if [[ "${BUILD_DIR}" != /* ]]; then
    BUILD_DIR="${ROOT_DIR}/${BUILD_DIR}"
fi

if [[ ! -f "${BUILD_DIR}/compile_commands.json" ]]; then
    echo "ERROR: compile_commands.json not found in ${BUILD_DIR}."
    echo "Build the project first with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON."
    exit 1
fi

CLANG_TIDY="${CLANG_TIDY:-clang-tidy-19}"
if ! command -v "${CLANG_TIDY}" &>/dev/null; then
    CLANG_TIDY="clang-tidy"
fi

if ! command -v "${CLANG_TIDY}" &>/dev/null; then
    echo "ERROR: clang-tidy not found. Install it (e.g. sudo apt install clang-tidy-19)." >&2
    exit 1
fi

echo "Using: $(command -v "${CLANG_TIDY}")"

# Only lint files actually compiled in this build configuration (those
# present in compile_commands.json). Files belonging to optional backends
# (software, vulkan, dlt, ...) or plugin-only sources are skipped when the
# corresponding feature is off, avoiding spurious errors from unavailable
# headers and mis-resolved compilation flags.
#
# Further exclude vendored Flutter embedder sources (client_wrapper,
# public) and third_party/.
mapfile -t FILES < <(
    python3 - "${BUILD_DIR}/compile_commands.json" "${ROOT_DIR}" <<'PY'
import json
import os
import sys

cc_path, root = sys.argv[1], os.path.realpath(sys.argv[2])
with open(cc_path) as fh:
    entries = json.load(fh)

excludes = (
    os.path.join(root, "third_party") + os.sep,
    os.path.join(root, "shell", "platform", "homescreen", "client_wrapper")
    + os.sep,
    os.path.join(root, "shell", "platform", "homescreen", "public") + os.sep,
)

seen = set()
for entry in entries:
    path = os.path.realpath(
        entry["file"]
        if os.path.isabs(entry["file"])
        else os.path.join(entry.get("directory", ""), entry["file"])
    )
    if not path.startswith(os.path.join(root, "shell") + os.sep):
        continue
    if any(path.startswith(ex) for ex in excludes):
        continue
    if not path.endswith((".cc", ".cpp")):
        continue
    if path in seen:
        continue
    seen.add(path)
    print(path)
PY
)

if [[ ${#FILES[@]} -eq 0 ]]; then
    echo "ERROR: no shell/ source files found in compile_commands.json." >&2
    exit 1
fi

printf '%s\n' "${FILES[@]}" | sort | \
    xargs "${CLANG_TIDY}" -p "${BUILD_DIR}" --warnings-as-errors='*' 2>&1

echo "clang-tidy passed."