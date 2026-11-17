#!/bin/bash
# Publish the browser archive package-artifacts.sh made.
#
#   publish-artifacts.sh <dir>     <dir> holds build_id and <build id>.tar.xz
#
# Two places:
#
# 1. Replay's S3 buckets, with the commands upstream's
#    upload_build_artifacts.mjs (replayio/chromium) runs for a Linux build
#    (uploadToAllBuckets and uploadArchives): the archive goes to builds/ in
#    the dev and prod buckets, and from the prod bucket to the website
#    bucket's downloads/, which is https://static.replay.io/downloads/.
#    What that script also does and this does not: build and upload a symbols
#    archive (it needs a backend checkout beside the tree), and check the
#    build id against one computed from the driver revision.
#
# 2. A GitHub release on the repository being built, tagged with the build
#    id at the commit built, with the archive as its asset. It needs a token
#    in GITHUB_TOKEN with contents:write on the repository; without one this
#    part is skipped and says so.
#
# It runs on the agent, not in the build image: it needs the agent's AWS
# identity and the buildkite-agent binary.
set -euo pipefail
dir=${1:?usage: publish-artifacts.sh <dir>}
build_id=$(cat "$dir/build_id")
archive="$dir/$build_id.tar.xz"
test -s "$archive"
name=$(basename "$archive")
repo=${PUBLISH_GITHUB_REPO:-BLamy/chromium}
commit=${BUILDKITE_COMMIT:-}

dev_bucket=recordreplay-us-east-2-dev
bucket=${RECORDREPLAY_BUCKET:-recordreplay-us-east-2}
website=recordreplay-website
url="https://static.replay.io/downloads/$name"
note=""
failed=0

echo "--- publish $name ($(stat -c %s "$archive") bytes)"
sha256sum "$archive" | cut -d' ' -f1 > "$dir/sha256"
echo "sha256 $(cat "$dir/sha256")"

echo "--- static.replay.io"
s3_ok=0
if ! command -v aws >/dev/null; then
  echo "no aws CLI on this agent"
elif ! aws sts get-caller-identity --query Arn --output text; then
  echo "this agent has no AWS identity"
else
  if aws s3 cp --acl bucket-owner-full-control "$archive" "s3://$dev_bucket/builds/$name" &&
     aws s3 cp --acl bucket-owner-full-control "$archive" "s3://$bucket/builds/$name" &&
     aws s3 cp --cache-control max-age=3600 "s3://$bucket/builds/$name" "s3://$website/downloads/$name"; then
    # The address answers before this says it was published.
    if curl -fsSI "$url" | head -1; then
      s3_ok=1
    else
      echo "uploaded, but $url does not answer"
    fi
  fi
fi
if [ "$s3_ok" = 1 ]; then
  note="$note* static.replay.io: [$name]($url)\n"
else
  note="$note* static.replay.io: **not published** (see this job's log)\n"
  failed=1
fi

echo "--- GitHub release on $repo"
gh_ok=0
if [ -z "${GITHUB_TOKEN:-}" ]; then
  echo "no GITHUB_TOKEN: the release is skipped"
else
  api="https://api.github.com/repos/$repo"
  auth=(-H "Authorization: Bearer $GITHUB_TOKEN" -H "Accept: application/vnd.github+json")
  body=$(printf '{"tag_name":"%s","target_commitish":"%s","name":"%s","body":"Replay Chromium 158, Linux x64, built by Buildkite build %s.\\n\\nsha256 `%s`","prerelease":true}' \
    "$build_id" "$commit" "$build_id" "${BUILDKITE_BUILD_NUMBER:-?}" "$(cat "$dir/sha256")")
  release=$(curl -fsS "${auth[@]}" "$api/releases/tags/$build_id" || curl -fsS "${auth[@]}" -X POST "$api/releases" -d "$body" || true)
  release_id=$(printf '%s' "$release" | sed -n 's/^  "id": \([0-9]*\),$/\1/p' | head -1)
  if [ -n "$release_id" ]; then
    if curl -fsS "${auth[@]}" -H "Content-Type: application/x-xz" -X POST \
         --data-binary "@$archive" \
         "https://uploads.github.com/repos/$repo/releases/$release_id/assets?name=$name" -o /dev/null; then
      gh_ok=1
    fi
  fi
fi
gh_url="https://github.com/$repo/releases/download/$build_id/$name"
if [ "$gh_ok" = 1 ]; then
  note="$note* GitHub release: [$name]($gh_url)\n"
elif [ -z "${GITHUB_TOKEN:-}" ]; then
  note="$note* GitHub release: skipped, this agent has no GITHUB_TOKEN\n"
else
  note="$note* GitHub release: **failed** (see this job's log)\n"
  failed=1
fi

if command -v buildkite-agent >/dev/null && [ -n "${BUILDKITE_JOB_ID:-}" ]; then
  printf "# Chromium 158 linux (x86_64): \`%s\`\n\n$note\nsha256 \`%s\`\n" "$build_id" "$(cat "$dir/sha256")" |
    buildkite-agent annotate --context publish --style "$([ "$failed" = 0 ] && echo success || echo warning)"
fi
exit "$failed"
