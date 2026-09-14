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

# True when <cardN> really is vkms, rather than merely wearing its connector
# names.
#
# The connector signature alone is ambiguous on exactly one rig that matters:
# Hyper-V's synthetic display calls its connector Virtual-1 too, so on an Azure
# CI runner both the vkms card and the VM's own display match the name, and a
# first-match search returns whichever enumerated first. It has been returning
# the right one there by luck of ordering.
#
# The device link is the stable discriminator. vkms registers on the faux bus,
# so cardN/device resolves to /sys/devices/faux/vkms; before that conversion it
# was a platform device whose driver symlink read "vkms". Accept either. The
# path survives the rename that made the driver name untrustworthy, because it
# is named for the device rather than for the bus glue.
ihs_is_vkms_card() {  # ihs_is_vkms_card <cardN>
    local card="$1" dev drv
    dev="$(readlink -f "/sys/class/drm/${card}/device" 2>/dev/null)" || return 1
    [ -n "$dev" ] && [ "$(basename "$dev")" = "vkms" ] && return 0
    drv="$(basename "$(readlink -f "/sys/class/drm/${card}/device/driver" \
           2>/dev/null)" 2>/dev/null)"
    [ "$drv" = "vkms" ]
}

# Echo /dev/dri/cardN for a vkms card with at least <min_connectors> scanout
# connectors (default 1), or return 1.
#
# vkms advertises connectors named "cardN-Virtual-M", which is the cheap filter,
# and ihs_is_vkms_card then confirms the card is actually vkms. The name match
# is kept as the first test because it is what the rest of this file reasons
# about, and because it rejects real GPUs without touching the filesystem twice;
# the confirmation is what stops Hyper-V's synthetic display from answering to
# it on a CI runner.
#
# Neither test is a match on the driver symlink alone, whose name has changed
# across kernel versions (platform -> faux_driver) and would silently stop
# matching again on the next rename.
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
        # card[0-9]* also globs the connector child nodes (card0-Virtual-1),
        # which are not cards. They are rejected by the connector-name test
        # below -- "card0-Virtual-1-Virtual-*" matches nothing -- rather than
        # here: a connector's device/ is a symlink back to its card, so it is a
        # directory and passes this test. This guards only against a card[0-9]*
        # entry with no device/ at all.
        [ -d "$c/device" ] || continue
        card="$(basename "$c")"
        compgen -G "/sys/class/drm/${card}-Virtual-*" >/dev/null || continue
        ihs_is_vkms_card "$card" || continue
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
