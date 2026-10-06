# shellcheck shell=bash
#
# fd_count.sh — open-fd accounting for the KMS harnesses.
#
# Sourced, not executed. #593 leaked one sync_file per explicit-sync submit and
# said nothing in the log: it surfaced as a ~100 ms stall each time the fd table
# crossed a power of two (growing it is RCU-synchronized), and then as the 1024
# soft limit about half a minute into a 30 fps stream. It was found by hand on a
# Pi 4 with `ls /proc/<pid>/fd`, because no test reaches the code it lives in.
#
# Two rules the callers encode:
#
#   - growth across a steady-state run, never an absolute count. The engine and
#     EGL open fds lazily, so a fixed ceiling flakes on a cold start.
#   - sample the final count while the process is still up. Teardown closes
#     things, which masks exactly what this is looking for.

# Open fds held by $1. Fails (rc 1) when /proc is not readable for that pid,
# which is a skip rather than a leak -- the caller warns and drops the check.
count_fds() {
    local dir="/proc/$1/fd"
    [[ -r "$dir" ]] || return 1
    find "$dir" -mindepth 1 -maxdepth 1 2>/dev/null | wc -l
}

# Read $1's fd count every $2 seconds until it stops climbing steeply -- two
# consecutive reads within $4 of each other -- or $3 reads have been taken, and
# echo the last count either way. Fails (rc 1) only when /proc closes.
#
# This is the other half of "growth, never an absolute count": growth only means
# anything measured from a settled state, and a fixed grace period does not
# establish one. On a loaded CI runner a 4 s grace left the baseline at 8 fds
# against a settled ~58, so ordinary lazy opening by the engine and EGL read as
# fds+32 and failed a backend that leaks nothing. The tell was that the final
# count, 40, was still *below* the settled figure: it was climbing towards steady
# state, not past it.
#
# It always returns a baseline rather than giving up, which is what keeps this
# from hiding the bug it exists to catch. A startup ramp flattens within a read
# or two, so the baseline lands on the plateau and growth comes out near zero. A
# real leak never flattens, so the reads run out, the baseline is simply the last
# one, and the leak keeps accruing over the measurement window that follows --
# which is exactly what the growth limit then catches. Equality is too strict for
# either case: a live shell's count jitters by one or two as it works.
settle_fds() {
    local pid="$1" step="$2" tries="$3" delta="$4"
    local prev="" cur="" i
    for (( i = 0; i < tries; i++ )); do
        cur="$(count_fds "$pid")" || return 1
        cur=$(( cur ))
        if [[ -n "$prev" ]] && (( cur - prev <= delta && prev - cur <= delta )); then
            break
        fi
        prev="$cur"
        sleep "$step"
    done
    [[ -n "$cur" ]] || return 1
    printf '%s\n' "$cur"
}
