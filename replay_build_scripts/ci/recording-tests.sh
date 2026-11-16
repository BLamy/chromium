#!/bin/bash
# Run the backend's chromium-recording suite on the browser this build made.
#
#   recording-tests.sh <dir with the build's .tar.xz> <out dir for logs>
#
# It stands in for upstream's "Chromium Recording Tests" step (replayio/chromium
# .buildkite/pipeline.yml), which takes the browser from S3 by build ID and the
# engine from a released linker. This pipeline has neither: the browser is the
# build step's Buildkite artifact, and the engine and driver are built here
# from the backend branch under test.
#
#   REPLAY_BACKEND_REF   the branch or commit of replayio/backend to test with
#   REPLAY_TEST_MCP      1 to also call the MCP tools on every case's recording
#   RECORDING_WORKERS    Playwright workers (default 1, the suite's own setting:
#                        a repaint has 2 s, and with two workers it ran out)
#   RECORDING_DIAG_GREP  cases to run first, many times over, with the stacks of
#                        any renderer that outlives its browser written to diag/
#   RECORDING_DIAG_REPEAT  how many times each (default 12)
#   RECORDING_SKIP_SUITES  1 to stop after the diagnostic pass
#
# Everything runs in one container with a memory and CPU limit, so that a suite
# that goes wrong cannot take the agent with it. It is privileged because two
# cases make a time namespace with `unshare`.
set -euo pipefail
artifacts=$(readlink -f "$1")
out=$(readlink -f "$2")
: "${REPLAY_BACKEND_REF:?}"
work=$(mktemp -d /tmp/crt158.XXXXXX)
name="chromium158-recording-${BUILDKITE_JOB_ID:-$$}"
cleanup() {
  docker rm -f "$name" > /dev/null 2>&1 || true
  # Files in it were made by root in the container.
  docker run --rm -v "$work:/w" debian:bookworm-slim sh -c 'rm -rf /w/* /w/.[!.]*' > /dev/null 2>&1 || true
  rm -rf "$work"
}
trap cleanup EXIT

echo "--- backend at $REPLAY_BACKEND_REF"
git init -q "$work/backend"
git -C "$work/backend" remote add origin git@github.com:replayio/backend.git
git -C "$work/backend" fetch -q --depth 1 origin "$REPLAY_BACKEND_REF"
git -C "$work/backend" checkout -q FETCH_HEAD
rev=$(git -C "$work/backend" rev-parse --short=12 HEAD)
node_version=$(sed 's/^v//' "$work/backend/.nvmrc")
echo "backend $rev, node $node_version"

echo "--- browser"
mkdir -p "$work/browser" "$work/logs"
tar -C "$work/browser" -xJf "$artifacts"/*.tar.xz
test -x "$work/browser/replay-chromium/chrome"
# Two cases start the browser through `unshare`, as a user that is not root:
# mktemp makes the directory for its owner only, and the browser was not found.
chmod 755 "$work"
chmod -R a+rX "$work/browser"

cat > "$work/inner.sh" <<'INNER'
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive HOME=/root CI=1 PLAYWRIGHT_HEADLESS=true TZ=UTC
echo "--- packages"
apt-get update -qq > /dev/null
apt-get install -y -qq build-essential zlib1g-dev python3 xz-utils procps util-linux git > /w/logs/apt.log 2>&1
cd /w/backend
echo "--- pnpm install"
npx --yes pnpm@10.11.0 install --frozen-lockfile > /w/logs/pnpm-install.log 2>&1 || { tail -30 /w/logs/pnpm-install.log; exit 1; }
# The libraries a Chromium needs, as Playwright lists them for this distribution.
(cd test/chromium-recording && npx playwright install-deps chromium > /w/logs/install-deps.log 2>&1) || { tail -20 /w/logs/install-deps.log; exit 1; }
echo "--- engine and driver"
mkdir -p /w/backend/out
printf '#include "recording/Recording.h"\nconst char recordreplay::gLinkerVersion[] = "linker-linux-26552-%s";\n' "$BACKEND_REV" > /w/backend/out/LinkerVersion.cpp
for target in linker driver; do
  make -C src/cpp "$target" CXX=g++ PLATFORM=linux OUT_DIR=/w/backend/out -j"$(nproc)" EXTRA_CXXFLAGS=-Wformat-nonliteral > "/w/logs/$target-build.log" 2>&1 || {
    grep -E " error|Error " "/w/logs/$target-build.log" | head -40; exit 1; }
done
ls -la /w/backend/out/linker /w/backend/out/linux-recordreplay.so
cd test/chromium-recording
status=0
# A browser that hangs while closing is killed by Playwright after 30 s, and
# the case then fails on its unfinished recording with nothing to say why. This
# pass runs the named cases with that limit lifted, and a watcher that writes
# the stacks of a renderer that has outlived its browser and then kills it.
if [ -n "${RECORDING_DIAG_GREP:-}" ]; then
  echo "--- diagnostic pass: $RECORDING_DIAG_GREP"
  apt-get install -y -qq gdb > /w/logs/apt-gdb.log 2>&1
  mkdir -p /w/logs/diag
  n=0
  for f in $(find /w/backend/node_modules -path "*playwright-core/lib/coreBundle.js"); do
    sed -i 's/closeOrKill(options.__testHookBrowserCloseTimeout || DEFAULT_PLAYWRIGHT_TIMEOUT)/closeOrKill(9e5)/' "$f"
    n=$((n + $(grep -c "closeOrKill(9e5)" "$f")))
  done
  echo "close limit lifted in $n places"
  (
    # A renderer whose browser has gone and that is still there on the next look: it is what leaves a
    # recording unfinished. Its stacks are written, and it is killed so the case goes on to fail as usual.
    orphans=" "
    dumps=0
    while :; do
      now=" "
      for pid in $(pgrep -x chrome || true); do
        args=$(tr "\0" " " < "/proc/$pid/cmdline" 2> /dev/null || true)
        case "$args" in *--type=renderer*) ;; *) continue ;; esac
        ppid=$(awk '{print $4}' "/proc/$pid/stat" 2> /dev/null || true)
        pcomm=$(cat "/proc/$ppid/comm" 2> /dev/null || true)
        if [ "$pcomm" = "chrome" ]; then continue; fi
        now="$now$pid "
        case "$orphans" in
          *" $pid "*)
            dumps=$((dumps + 1))
            d=/w/logs/diag/orphan-$dumps
            mkdir -p "$d"
            ps -eo pid,ppid,pgid,etimes,stat,pcpu,rss,args | cut -c1-300 > "$d/ps.txt"
            printf '%s\n' "$args" | cut -c1-800 > "$d/cmdline-$pid.txt"
            for t in /proc/$pid/task/*; do
              echo "$(basename "$t") $(cat "$t/comm" 2> /dev/null) $(awk '{print $3}' "$t/stat" 2> /dev/null) syscall: $(cat "$t/syscall" 2> /dev/null)"
            done > "$d/threads-$pid.txt" 2>&1
            if [ "$dumps" -le 6 ]; then
              timeout 600 gdb -batch -nx -p "$pid" -ex "set pagination off" -ex "thread apply all bt 60" > "$d/bt-$pid.txt" 2>&1 || true
            fi
            for r in /w/backend/test/chromium-recording/test-results/*/recording/recordings.log; do
              if grep -q writeStarted "$r" 2> /dev/null; then echo "== $r"; cat "$r"; fi
            done > "$d/recordings-log.txt" 2>&1
            kill -9 "$pid" 2> /dev/null || true
            ;;
        esac
      done
      orphans="$now"
      sleep 3
    done
  ) &
  watcher=$!
  code=0
  env RECORD_REPLAY_DRIVER=/w/backend/out/linux-recordreplay.so \
      REPLAY_CHROMIUM_EXECUTABLE_PATH=/w/browser/replay-chromium/chrome \
      REPLAY_LINKER_PATH=/w/backend/out/linker \
      PLAYWRIGHT_JSON_OUTPUT_FILE=/w/logs/results-diag.json \
      npx playwright test --config=playwright.config.ts --reporter=line,json --workers=2 \
      --timeout=900000 --repeat-each="${RECORDING_DIAG_REPEAT:-12}" -g "$RECORDING_DIAG_GREP" \
      > /w/logs/playwright-diag.log 2>&1 || code=$?
  kill "$watcher" 2> /dev/null || true
  grep -E "^  [0-9]+\) \[|^    Error|^    ErrorChain|^  [0-9]+ (passed|failed|flaky|skipped)" /w/logs/playwright-diag.log | cut -c1-300 | tail -60
  ls /w/logs/diag
  tar -cJf /w/logs/diag.tar.xz -C /w/logs diag
  rm -rf /w/logs/diag
  echo "diagnostic pass exit $code"
  if [ -n "${RECORDING_SKIP_SUITES:-}" ]; then exit "$code"; fi
  # The suites run with Playwright as it is.
  for f in $(find /w/backend/node_modules -path "*playwright-core/lib/coreBundle.js"); do
    sed -i 's/closeOrKill(9e5)/closeOrKill(options.__testHookBrowserCloseTimeout || DEFAULT_PLAYWRIGHT_TIMEOUT)/' "$f"
  done
fi
# One pass of the suite as it is, then one with every recording also read
# through the MCP tools, so a failure can be told from the other kind.
run_suite() {
  local name=$1 mcp=$2 code=0
  echo "--- chromium-recording: $name"
  env RECORD_REPLAY_DRIVER=/w/backend/out/linux-recordreplay.so \
      REPLAY_CHROMIUM_EXECUTABLE_PATH=/w/browser/replay-chromium/chrome \
      REPLAY_LINKER_PATH=/w/backend/out/linker \
      REPLAY_TEST_MCP="$mcp" \
      PLAYWRIGHT_JSON_OUTPUT_FILE="/w/logs/results-$name.json" \
      npx playwright test --config=playwright.config.ts --reporter=line,json --workers="${RECORDING_WORKERS:-1}" \
      > "/w/logs/playwright-$name.log" 2>&1 || code=$?
  grep -E "^  [0-9]+\) \[|^    Error|^    ErrorChain|^    \+   \"|Tearing down|^  [0-9]+ (passed|failed|flaky|skipped)" "/w/logs/playwright-$name.log" | cut -c1-300 | tail -80
  if [ -d test-results ]; then
    find test-results -name mcp-sweep.json | tar -cJf "/w/logs/mcp-sweeps-$name.tar.xz" -T - 2> /dev/null || true
  fi
  return "$code"
}
run_suite plain "" || status=$?
if [ -n "${REPLAY_TEST_MCP:-}" ]; then
  run_suite mcp 1 || status=$?
fi
exit "$status"
INNER

status=0
docker run --rm --name "$name" --privileged --security-opt seccomp=unconfined \
  --memory=22g --memory-swap=22g --cpus=7 --pids-limit=4096 --shm-size=2g \
  -e BACKEND_REV="$rev" -e REPLAY_TEST_MCP="${REPLAY_TEST_MCP:-}" -e RECORDING_WORKERS="${RECORDING_WORKERS:-1}" \
  -e RECORDING_DIAG_GREP="${RECORDING_DIAG_GREP:-}" -e RECORDING_DIAG_REPEAT="${RECORDING_DIAG_REPEAT:-}" \
  -e RECORDING_SKIP_SUITES="${RECORDING_SKIP_SUITES:-}" \
  -v "$work:/w" "node:${node_version}-bookworm" bash /w/inner.sh || status=$?
mkdir -p "$out"
cp -r "$work/logs/." "$out/" 2> /dev/null || true
gzip -f "$out"/*.log 2> /dev/null || true
exit "$status"
