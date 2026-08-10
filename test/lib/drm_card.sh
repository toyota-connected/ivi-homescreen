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

# Echo /dev/dri/cardN for the vkms card, or return 1.
#
# vkms is identified by its connector signature: it always advertises
# connectors named "cardN-Virtual-M", and real GPUs do not. That is deliberately
# not a match on the driver symlink, whose name has changed across kernel
# versions (platform -> faux_driver) and would silently stop matching again on
# the next rename.
ihs_find_vkms_card() {
    local c card
    for c in /sys/class/drm/card[0-9]*; do
        # Skip the connector/encoder child nodes, which have no device/ dir.
        [ -d "$c/device" ] || continue
        card="$(basename "$c")"
        if compgen -G "/sys/class/drm/${card}-Virtual-*" >/dev/null; then
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
# Writeback connectors are excluded: they are capture targets, not scanout ones,
# so nothing can be pinned to them for display.
ihs_connectors_of() {  # ihs_connectors_of <cardN>
    local card="$1" c
    for c in /sys/class/drm/"${card}"-*; do
        [ -e "$c/status" ] || continue
        case "$c" in *Writeback*) continue ;; esac
        basename "$c" | sed "s/^${card}-//"
    done
}
