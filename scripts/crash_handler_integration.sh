#!/usr/bin/env bash
# Crash-handler integration test.
#
# Builds THIS source tree (the checkout, not a pinned workspace) with the crash
# handler via emb's native `local` build, then runs the intentionally-crashing
# binary against a fake Sentry and verifies the crash report was delivered.
#
# emb stages the sentry-native augment (+ crashpad_handler) into a per-workspace
# overlay and fetches the Flutter engine SDK, so this needs only emb + a Flutter
# SDK on PATH — no meta-flutter workspace checkout.
#
# The fake Sentry accepts crashpad's minidump upload: crashpad POSTs the crash
# to `/api/<project>/minidump/`, an endpoint Kent (the usual fake Sentry) does
# not serve — so we run a tiny minidump-aware stand-in instead.
#
# Env (all optional):
#   EMB         emb executable (default: `emb` on PATH)
#   APP         Flutter app to bundle (default: a throwaway `flutter create`;
#               the binary crashes at startup, so the app content is irrelevant)
#   MOCK_PORT   fake Sentry port (default: 5000)

set -euo pipefail

EMB="${EMB:-emb}"
MOCK_PORT="${MOCK_PORT:-5000}"
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# ── A minimal app for the bundle ───────────────────────────────────────────
APP="${APP:-}"
if [[ -z "$APP" ]]; then
  APP="$(mktemp -d)/crashapp"
  flutter create "$APP" >/dev/null
fi

# ── Build embedder (crash handler + integration crash) + runnable bundle ───
# Run from the source tree (emb cross .) so the -D define overrides apply — an
# absolute source path selects a different build key and can reuse a cached
# no-crash build. Capture emb's "runnable → <path>" line for the exact bundle.
cd "$SRC"
BUILD_LOG="$(mktemp)"
"$EMB" cross . --backend software \
  -D BUILD_CRASH_HANDLER=ON -D INTEGRATION_TEST_CRASH_HANDLER=ON \
  --build --app "$APP" --mode debug 2>&1 | tee "$BUILD_LOG"

RUN="$(grep -a 'runnable' "$BUILD_LOG" | grep -aoE '/[^[:space:]]+/runnable' | tail -1)"
[[ -n "$RUN" && -x "$RUN/homescreen" ]] || {
  echo "error: emb runnable bundle not found" >&2
  exit 2
}
echo "runnable bundle: $RUN"

# ── Fake Sentry (minidump-aware) ───────────────────────────────────────────
HITS="$(mktemp)"
python3 - "$MOCK_PORT" "$HITS" <<'PY' >/dev/null 2>&1 &
import http.server, socketserver, sys, pathlib
port = int(sys.argv[1]); hits = pathlib.Path(sys.argv[2]); hits.write_text("")
class H(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        try:
            self.rfile.read(int(self.headers.get("Content-Length", "0") or 0))
        except Exception:
            pass
        with hits.open("a") as f:
            f.write(self.path + "\n")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(b'{"id":"x"}')
    def log_message(self, *a):
        pass
socketserver.TCPServer(("127.0.0.1", port), H).serve_forever()
PY
MOCK_PID=$!
cleanup() {
  kill "$MOCK_PID" 2>/dev/null || true
  pkill -9 -x crashpad_handler 2>/dev/null || true
  pkill -9 -x homescreen 2>/dev/null || true
}
trap cleanup EXIT
sleep 2

# ── Crash, then confirm the crash report reached Sentry ────────────────────
run_and_verify() {
  set +e
  ( cd "$RUN" && SENTRY_DSN="http://public@127.0.0.1:${MOCK_PORT}/1" \
      LD_LIBRARY_PATH="$RUN/lib" timeout -k 5 60 ./homescreen -b . )
  local ec=$?
  set -e
  echo "homescreen exit: ${ec}"
  if [[ "${ec}" != 139 ]]; then
    echo "error: expected crash exit 139 (SIGSEGV), got ${ec}" >&2
    return 1
  fi
  sleep 12  # crashpad uploads the minidump out-of-process, asynchronously
  if grep -q 'minidump' "$HITS"; then
    echo "crash report (minidump) delivered to Sentry:"
    cat "$HITS"
    return 0
  fi
  echo "error: no minidump crash report received by the fake Sentry" >&2
  cat "$HITS" >&2
  return 1
}

for attempt in 1 2; do
  if run_and_verify; then
    echo "crash-handler integration test PASSED"
    exit 0
  fi
  if [[ "${attempt}" == 2 ]]; then
    echo "error: crash-handler verification failed after ${attempt} attempts" >&2
    exit 1
  fi
  echo "attempt ${attempt} failed, retrying..."
  : > "$HITS"
done
