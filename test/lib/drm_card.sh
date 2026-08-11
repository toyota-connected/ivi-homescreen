# shellcheck shell=bash
#
# drm_card.sh — shared DRM node/connector discovery for the test harnesses.
#
# Sourced, not executed. Every harness that drives KMS needs the same two
# answers ("which card?", "which connectors?"), and getting the first one wrong
# is not a cosmetic bug: these harnesses take DRM master and drive a modeset, so
# a harness that guesses a card number can blank a developer's real display.
#
# Card numbering is not stable. vkms lands wherever it lands relative to any
# real GPU, and moves as other DRM drivers load or are rebuilt into the kernel,
# so no numeric default is correct anywhere but the machine it was written on.
# The rule these helpers encode is therefore:
#
#   - auto-detection resolves vkms and nothing else, so it is always safe;
#   - targeting real hardware requires the caller to name the node explicitly,
#     which is the operator saying so out loud;
#   - when neither applies, the harness skips rather than guessing.
#
# Usage:
#   source "$(dirname "${BASH_SOURCE[0]}")/lib/drm_card.sh"
#   card="$(ihs_find_vkms_card)" || card=""
#   mapfile -t conns < <(ihs_connectors_of "$(basename "$card")")

# Echo /dev/dri/cardN for a vkms card with at least <min_connectors> scanout
# connectors (default 1), or return 1.
#
# vkms is identified by its connector signature: it always advertises
# connectors named "cardN-Virtual-M", and real GPUs do not. That is deliberately
# not a match on the driver symlink, whose name has changed across kernel
# versions (platform -> faux_driver) and would silently stop matching again on
# the next rename.
#
# The connector count matters because there is routinely more than one vkms
# card. `modprobe vkms` creates a default instance with a single output, and
# vkms_dual.sh then provisions a second instance with two -- so a bare
# first-match search finds the single-output card and a harness needing two
# outputs skips, with the card it wanted sitting right there. Callers say how
# many they need.
ihs_find_vkms_card() {  # ihs_find_vkms_card [min_connectors]
    local min="${1:-1}" c card count
    for c in /sys/class/drm/card[0-9]*; do
        # Skip the connector/encoder child nodes, which have no device/ dir.
        [ -d "$c/device" ] || continue
        card="$(basename "$c")"
        compgen -G "/sys/class/drm/${card}-Virtual-*" >/dev/null || continue
        count="$(ihs_connectors_of "$card" | wc -l)"
        if [ "$count" -ge "$min" ]; then
            echo "/dev/dri/${card}"
            return 0
        fi
    done
    return 1
}

# Echo the scanout connector names on <cardN>, one per line, in sysfs order.
#
# Names are card-specific -- Virtual-N on vkms, HDMI-A-1/DP-1 on a real GPU --
# so a harness must read them off the card it resolved rather than defaulting to
# either set, which is wrong on the other.
#
# Only connectors reporting "connected" are listed. A disconnected connector
# still appears in sysfs, and pinning a bundle to one produces a modeset failure
# deep in the backend rather than an obvious "there is no second display here"
# -- which is the actual situation on, say, a Pi with one DSI panel and two
# empty HDMI ports. vkms reports its virtual outputs as connected, so this reads
# the same on both rigs.
#
# Writeback connectors are excluded regardless: they are capture targets, not
# scanout ones, so nothing can be pinned to them for display.
ihs_connectors_of() {  # ihs_connectors_of <cardN>
    local card="$1" c
    for c in /sys/class/drm/"${card}"-*; do
        [ -e "$c/status" ] || continue
        case "$c" in *Writeback*) continue ;; esac
        [ "$(cat "$c/status" 2>/dev/null)" = "connected" ] || continue
        basename "$c" | sed "s/^${card}-//"
    done
}
