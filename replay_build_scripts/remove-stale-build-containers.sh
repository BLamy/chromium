#!/bin/bash
# A build container still on this agent is left over from a job that was
# cancelled or killed: the agent runs one job at a time. Remove it before
# anything touches a checkout it would keep writing to (overlapping ninja runs
# corrupted out/Release's ninja state once, replayio/chromium#1540).
#
# Only this pipeline's containers: build-in-container.sh names them
# chromium158-build-<key>-<job>. Upstream's script also removes every container
# of the Chromium 108 pipeline (named chromium-build-*, or made from the
# chromium-build-new image); this one must never do that, on any agent.

set -o pipefail

key=${1:-}
list_build_containers() {
  docker ps --all --quiet --filter "name=^/?chromium158-build-${key:+$key-}" | sort -u
}

stale=$(list_build_containers) || {
  echo "Listing build containers failed" >&2
  exit 1
}
if [ -z "$stale" ]; then
  exit 0
fi
echo "Removing stale build containers:" $stale
# Fails for a container that is already being removed, e.g. by its own job's
# cancel handler, so wait for them to be gone instead.
docker rm --force $stale
for _ in $(seq 30); do
  stale=$(list_build_containers) || exit 1
  if [ -z "$stale" ]; then
    exit 0
  fi
  sleep 2
done
echo "Build containers still present:" $(list_build_containers) >&2
exit 1
