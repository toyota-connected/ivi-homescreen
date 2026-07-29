#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ivi-homescreen contributors
#
# clang_tidy.sh — run clang-tidy-19 across the project sources.
#
# Usage:
#   scripts/clang_tidy.sh [BUILD_DIR]
#   scripts/clang_tidy.sh --shard <index>/<total> [BUILD_DIR]
#
# BUILD_DIR defaults to "build" and must contain a compile_commands.json
# (configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON).
#
# --shard <index>/<total> — only lint files in shard <index> (0-based) out of
#   <total> shards. Used in CI to split the work across multiple runners.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

# Parse arguments
SHARD_INDEX=""
SHARD_TOTAL=""
POSITIONAL_ARGS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --shard)
            SHARD_INDEX="${2%%/*}"
            SHARD_TOTAL="${2##*/}"
            shift 2
            ;;
        *)
            POSITIONAL_ARGS+=("$1")
            shift
            ;;
    esac
done

BUILD_DIR="${POSITIONAL_ARGS[0]:-build}"
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
# (vulkan, dlt, ...) or plugin-only sources are skipped when the
# corresponding feature is off, avoiding spurious errors from unavailable
# headers and mis-resolved compilation flags.
#
# Further exclude vendored Flutter embedder sources (client_wrapper,
# public) and third_party/.
#
# If --shard was provided, split the sorted list into <total> groups and
# only emit the files belonging to shard <index>.
mapfile -t FILES < <(
    python3 - "${BUILD_DIR}/compile_commands.json" "${ROOT_DIR}" \
        "${SHARD_INDEX:-}" "${SHARD_TOTAL:-}" <<'PY'
import json
import os
import sys
from itertools import islice

cc_path, root = sys.argv[1], os.path.realpath(sys.argv[2])
shard_index = int(sys.argv[3]) if sys.argv[3] else None
shard_total = int(sys.argv[4]) if sys.argv[4] else None

with open(cc_path) as fh:
    entries = json.load(fh)

excludes = (
    os.path.join(root, "third_party") + os.sep,
    os.path.join(root, "shell", "platform", "homescreen", "client_wrapper")
    + os.sep,
    os.path.join(root, "shell", "platform", "homescreen", "public") + os.sep,
)

roots = (
    os.path.join(root, "shell") + os.sep,
    os.path.join(root, "shared") + os.sep,
)

seen = set()
file_list = []
for entry in entries:
    path = os.path.realpath(
        entry["file"]
        if os.path.isabs(entry["file"])
        else os.path.join(entry.get("directory", ""), entry["file"])
    )
    if not any(path.startswith(r) for r in roots):
        continue
    if any(path.startswith(ex) for ex in excludes):
        continue
    if not path.endswith((".cc", ".cpp")):
        continue
    if path in seen:
        continue
    seen.add(path)
    file_list.append(path)

# Sort for deterministic shard assignment.
file_list.sort()

# Split into shards if requested.
if shard_index is not None and shard_total is not None:
    files = list(islice(
        file_list, shard_index, len(file_list), shard_total
    ))
    if not files:
        print(f"no files for shard {shard_index}/{shard_total}", file=sys.stderr)
        sys.exit(0)

for path in files if shard_index is not None else file_list:
    print(path)
PY
)

if [[ ${#FILES[@]} -eq 0 ]]; then
    if [[ -n "${SHARD_INDEX}" ]]; then
        echo "No files for shard ${SHARD_INDEX}/${SHARD_TOTAL}."
        exit 0
    fi
    echo "ERROR: no shell/ source files found in compile_commands.json." >&2
    exit 1
fi

printf '%s\n' "${FILES[@]}" | sort | \
    xargs -P "$(nproc 2>/dev/null || echo 4)" "${CLANG_TIDY}" -p "${BUILD_DIR}" --warnings-as-errors='*' 2>&1

echo "clang-tidy passed."