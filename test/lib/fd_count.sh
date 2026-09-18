# shellcheck shell=bash
#
# fd_count.sh — open-fd accounting for the KMS harnesses.
#
# Sourced, not executed. #593 leaked one sync_file per explicit-sync submit and
# said nothing in the log: it surfaced as a ~100 ms stall each time the fd table
# crossed a power of two (growing it is RCU-synchronised), and then as the 1024
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
