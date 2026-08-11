#!/usr/bin/env bash
#
# vkms_harness_integration.sh — CI integration for the event-driven input harness
# and the present-path census. Builds the deterministic scroll_bench bundle with
# emb, points both harnesses at homescreen on a vkms card, and emits a
# RAN/SKIPPED summary to $GITHUB_STEP_SUMMARY.
#
# Prerequisites (set up by the workflow before this runs):
#   - homescreen built at ${IVI_BUILD}/shell/homescreen with the DRM-KMS-EGL and
#     software backends (the census exercises both).
#   - emb on PATH with a provisioned Flutter (emb flutter ...); FLUTTER_WORKSPACE set.
#   - vkms loaded with a Virtual connector.
#
# Coverage note: the census runs in self-check with the strict correctness gates
# (discarded==0, stalls==0) but a relaxed keep-up floor and a wider window,
# because hosted runners are shared and software-GL — their frame timing is not
# stable enough to gate on. The strict baseline diff
# (present_census.sh --check test/baselines/present_census.vkms.txt) is for
# self-hosted / hardware runners. Input injection scenarios need a writable
# /dev/uinput; on a hosted runner it stays root-only so they SKIP (honestly
# reported), and RUN where the runner can open it.
set -uo pipefail

IVI_SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IVI_BUILD="${IVI_BUILD:-${IVI_SRC}/build}"
APP_DIR="${IVI_SRC}/test/integration/scroll_bench"
BUNDLE_ROOT="${FLUTTER_WORKSPACE:-${IVI_SRC}}/bundle"

export HOMESCREEN="${IVI_BUILD}/shell/homescreen"
export LD_LIBRARY_PATH="${IVI_BUILD}/shared:${IVI_BUILD}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
[[ -x "${HOMESCREEN}" ]] || { echo "error: homescreen not at ${HOMESCREEN}"; exit 2; }

# ---------------------------------------------------------------------------
# Build the deterministic workload bundle. emb names the output
# scroll_bench-debug-<arch>; resolve it by glob rather than guessing the arch
# suffix (uname -i is "unknown" on some hosts while emb uses the machine arch).
# ---------------------------------------------------------------------------
echo "== building scroll_bench bundle =="
emb bundle --app-path "${APP_DIR}" -m debug --build
shopt -s nullglob
bundles=( "${BUNDLE_ROOT}"/scroll_bench-debug-* )
shopt -u nullglob
[[ ${#bundles[@]} -ge 1 ]] || { echo "error: no scroll_bench bundle under ${BUNDLE_ROOT}"; exit 2; }
export BUNDLE="${bundles[0]}"
echo "bundle: ${BUNDLE}"

# Append a section to the job summary (or stdout when run outside Actions).
emit() { if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then cat >>"${GITHUB_STEP_SUMMARY}"; else cat; fi; }

rc=0
run_harness() {  # run_harness <title> <cmd...>
    local title="$1"; shift
    local out="/tmp/${title// /_}.out"
    echo "== ${title} =="
    local status=0
    "$@" 2>&1 | tee "${out}" || status="${PIPESTATUS[0]}"
    [[ "${status}" -ne 0 ]] && rc=1
    {
        echo "### ${title}"
        echo
        echo '```'
        # The RESULTS lines ("  <name> <pass|fail|skip> — …") plus the tallies.
        grep -E '(pass|fail|skip) —|RAN:|backends:' "${out}" || echo "(no summary emitted)"
        echo '```'
        echo
        echo "exit: ${status}"
        echo
    } | emit
}

# ---------------------------------------------------------------------------
# Present-path census (self-check, relaxed timing for shared runners)
# ---------------------------------------------------------------------------
run_harness "present census" \
    env CENSUS_SECS=12 KEEPUP_MIN=0.30 "${IVI_SRC}/test/present_census.sh"

# ---------------------------------------------------------------------------
# Event-driven input harness (I2/I3 always; I1/I6 run only with writable uinput).
# Raise I2's idle wake ceiling: a shared, loaded runner accrues more idle
# context switches than a quiet box while staying far below a busy poll (~300/5s).
# ---------------------------------------------------------------------------
run_harness "event-driven input harness" \
    env IDLE_CTXT_MAX=150 "${IVI_SRC}/test/input_event_driven.sh"

# ---------------------------------------------------------------------------
# OSGi multi-bundle: two bundles -> two engines -> two connectors, critical
# first. Needs two connectors on one card and a binary built with
# ENABLE_OSGI=ON; the harness reports an honest skip when either is missing
# rather than failing, so it is wired in here from the start and goes live by
# flipping ENABLE_OSGI in the build step above it.
# ---------------------------------------------------------------------------
# DRM_DEVICE and the connector names are deliberately NOT passed: card
# numbering is not stable (vkms lands wherever it lands relative to any real
# GPU) and connector names are card-specific (Virtual-N on vkms, HDMI-A-1/DP-1
# on a real GPU). The harness auto-detects vkms by its connector signature and
# reads the connectors off that card, so nothing here has to guess. It skips
# honestly when there is no vkms card or the binary lacks ENABLE_OSGI.
run_harness "osgi multi-bundle" \
    env SOFTWARE_RENDER=1 "${IVI_SRC}/test/osgi_multi_bundle.sh"

echo "vkms harness integration: overall rc=${rc}"
exit "${rc}"
