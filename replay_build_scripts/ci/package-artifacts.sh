#!/bin/bash
# Package the browser this key's checkout built, as Buildkite artifacts.
#
#   package-artifacts.sh [<dir>]     default: ./build_id/linux/x86_64
#
# It stands in for upstream's upload-artifacts.sh (replayio/chromium), which
# runs upload_build_artifacts.mjs: that uploads the build to Replay's S3
# buckets, builds a symbols archive with the backend checkout and posts to the
# release service, none of which this pipeline has credentials or a backend
# for. What it keeps from it is the archive itself -- the same files, in the
# same replay-chromium/ directory, as <build id>.tar.xz -- and the build_id
# file, written where the pipeline's artifact_paths look, so the build is kept
# by Buildkite instead of S3.
set -euo pipefail
dir=$(dirname "$(readlink -f "$0")")
root=$("$dir/checkout.sh" require)
dest=${1:-build_id/linux/x86_64}

# In the image: it has xz, and the paths are the ones the build used. The
# staging directory is new each time, so nothing has to be removed first.
stage=artifacts-${BUILDKITE_JOB_ID:-$$}
"$dir/build-in-container.sh" --root "$root" -- bash -euo pipefail -c '
  stage=/chromium/$1
  out=/chromium/src/out/Release
  build_id=$(sed -n "s/^#define RECORD_REPLAY_BUILD_ID \"\(.*\)\"/\1/p" /chromium/src/base/record_replay_driver.h)
  test -n "$build_id"
  test -x "$out/chrome"
  mkdir -p "$stage/replay-chromium"
  cd "$out"
  # The files upload_build_artifacts.mjs copies for Linux (copyBuildFiles).
  for f in icudtl.dat v8_context_snapshot.bin vk_swiftshader_icd.json replay \
           chrome chrome_crashpad_handler *.pak *.pak.info *.so *.so.1 \
           locales replay-assets; do
    if [ -e "$f" ]; then
      cp -a "$f" "$stage/replay-chromium/"
    fi
  done
  cd "$stage"
  ls -la replay-chromium
  # Parallel, with reasonable compression, as upstream does.
  tar -c -I "xz -2 -T0" --remove-files -f "$build_id.tar.xz" replay-chromium
  echo "$build_id" > build_id
  ls -la' package "$stage"

mkdir -p "$dest"
mv "$root/$stage/"* "$dest/"
rmdir "$root/$stage"
echo "package-artifacts: $(cat "$dest/build_id") in $dest"
ls -la "$dest"
