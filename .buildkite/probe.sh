#!/bin/bash
# What a build agent has: cores, memory, disk, docker, git. Read-only.
echo "--- host"; hostname; uname -a; id; nproc; free -g | head -2
echo "--- disk"; df -h / "$HOME" /var/lib/docker /mnt/data 2>/dev/null
echo "--- tools"; git --version; docker --version 2>&1; node --version 2>&1; python3 --version 2>&1
echo "--- docker"; docker info 2>&1 | grep -E "Server Version|Docker Root Dir|Storage Driver|CPUs|Total Memory|Architecture" ; docker ps --format '{{.Names}} {{.Image}} {{.Status}}' 2>&1 | head -20
docker images --format '{{.Repository}}:{{.Tag}} {{.Size}}' 2>&1 | head -30
echo "--- home"; ls -la "$HOME" 2>&1 | head -40
echo "--- chromium checkouts"; ls -la "$HOME/chromium-checkouts" "$HOME/chromium" 2>&1 | head -40
echo "--- agent"; echo "$BUILDKITE_AGENT_NAME"; echo "$BUILDKITE_BUILD_CHECKOUT_PATH"; pwd; ps -eo comm | grep -c buildkite-agent
echo "--- load"; uptime
echo "--- net"; curl -sS -o /dev/null -w "github %{http_code}\n" https://github.com/BLamy/chromium.git/info/refs?service=git-upload-pack; curl -sS -o /dev/null -w "googlesource %{http_code}\n" https://chromium.googlesource.com/chromium/tools/depot_tools.git/info/refs?service=git-upload-pack; curl -sSI -o /dev/null -w "driver %{http_code}\n" https://static.replay.io/downloads/linux-recordreplay.tgz
