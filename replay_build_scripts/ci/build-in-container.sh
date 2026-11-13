#!/bin/bash
# Build a per-version checkout in the image this repo describes.
#
#   build-in-container.sh                      # build the key's checkout
#   build-in-container.sh --root <dir> -- ...  # run something else in it
#
# The only writable mount is one key's directory (checkout.sh), and depot_tools
# is the pin inside the image.
#
# Chromium 158 differences from upstream's script (replayio/chromium): the
# containers are named chromium158-build-*, so nothing that removes the 108
# pipeline's containers by name matches them; there is no shared git cache and
# no EngFlow cert, because 158 fetches without history and compiles on the
# agent; and the container runs at a low CPU priority, because the agents it
# runs on are shared.
#
# The mount point is /chromium for every key, so gn, ninja and reclient see the
# same paths they always have and nothing rebuilds because a directory moved.
set -euo pipefail
dir=$(dirname "$(readlink -f "$0")")

root=""
cmd=()
while [ $# -gt 0 ]; do
  case $1 in
    --root) root=${2:?--root needs a directory}; shift 2 ;;
    --) shift; cmd=("$@"); break ;;
    *) echo "usage: $0 [--root <dir>] [-- <command>]" >&2; exit 1 ;;
  esac
done
if [ -z "$root" ]; then
  root=$("$dir/checkout.sh" require)
fi
if [ ${#cmd[@]} -eq 0 ]; then
  # build.cjs, not upstream's build.js: 158's package.json says "type": "module",
  # and the build script is CommonJS.
  cmd=(node build.cjs)
fi
mkdir -p "$root/home"

image=$("$dir/build-image.sh" ref)
"$dir/build-image.sh" ensure >&2

# Named after the version and the job: cancelling the job can then remove the
# container, and the sweep for orphans can leave other versions' builds alone.
# --init: the image's command runs under a shell, which as PID 1 would ignore
# SIGTERM, and a cancelled job's build then kept running in the checkout
# (replayio/chromium#1540).
key=${root%/}
key=${key##*/}
name="chromium158-build-${key//[^A-Za-z0-9_.-]/-}-${BUILDKITE_JOB_ID:-$$}-$RANDOM"

args=(
  run --name "$name" --rm --init
  -v "$root:/chromium"
  -w /chromium/src
  # A compile uses every core for hours, on an agent that also runs other
  # things. A low share only matters under contention: everything else on the
  # machine gets the CPU first, and an otherwise idle machine is still used
  # fully.
  --cpu-shares 128
  # The image's user has this uid (build-image.sh); passing it explicitly means
  # the files a build writes belong to whoever ran it either way.
  --user "$(id -u):$(id -g)"
  -e HOME=/chromium/home
)
for var in BUILDKITE BUILDKITE_BRANCH BUILDKITE_PIPELINE_DEFAULT_BRANCH \
           LOCAL_DEVELOPER_BUILD_EXTENSION DRIVER_REVISION \
           REPLAY_NINJA_JOBS REPLAY_NINJA_KEEP_GOING; do
  if [ -n "${!var:-}" ]; then
    args+=(-e "$var")
  fi
done

if [ -n "${REPLAY_LOCAL_DRIVER_DIR:-}" ]; then
  driver=$(readlink -f "$REPLAY_LOCAL_DRIVER_DIR")
  args+=(-v "$driver:$driver:ro" -e "REPLAY_LOCAL_DRIVER_DIR=$driver")
fi

# Remove the container ourselves when the job is cancelled, rather than rely on
# the docker client passing the signal on before it is killed itself.
for signal in INT TERM HUP; do
  # shellcheck disable=SC2064
  trap "echo 'removing $name'; docker rm --force '$name' >/dev/null 2>&1 || true; exit 1" "$signal"
done

echo "\$ docker ${args[*]} $image ${cmd[*]}"
# In the background and waited for, not in the foreground: bash runs a trap
# only once the foreground child has exited, so a cancelled job's cleanup would
# wait for the build it is trying to stop.
docker "${args[@]}" "$image" "${cmd[@]}" &
wait $!
