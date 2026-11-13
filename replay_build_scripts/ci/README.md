# Chromium 158 builds on Buildkite

The pipeline is `chromium-158` (https://buildkite.com/replay/chromium-158). It
builds this repository, BLamy/chromium, for Linux x64 and keeps the browser as a
Buildkite artifact.

These scripts are upstream's (replayio/chromium at `4820c825b044`, "Build each
Chromium version in its own checkout and image", #1637), which build Chromium
108, adapted to 158. Upstream's own README explains the scheme at length; this
one says how this pipeline uses it and where it differs.

## It is separate from the Chromium 108 pipeline

`chromium-build`, the pipeline that builds 108 from replayio/chromium, runs on
the agent tagged `runtimeType=chromiumbuild`. This pipeline never runs there,
and nothing it makes can be mistaken for that pipeline's:

| | 108 (`chromium-build`) | 158 (`chromium-158`) |
|---|---|---|
| Agent | `runtimeType=chromiumbuild`, `queue=runtime` | `2xlarge=true`, `build-runtime=true`, `queue=default` |
| Checkout | `~/chromium-checkouts/m108` | `~/chromium-checkouts/m158` |
| Image | `chromium-build:<hash>` | `chromium158-build:<hash>-u<uid>` |
| Containers | `chromium-build-<key>-<job>` | `chromium158-build-<key>-<job>-<n>` |

`remove-stale-build-containers.sh` here removes only `chromium158-build-*`
containers. Upstream's also removes every `chromium-build-*` container and
everything made from the `chromium-build-new` image; that was taken out, so
this script is safe on any agent.

## What a job does

The pipeline's configuration in Buildkite is one step that fetches
`.buildkite/pipeline.yml` by URL at the commit being built and uploads it. No
step checks the repository out: a clone is tens of GB. The build step then:

1. `bootstrap.sh`: fetches this commit's `replay_build_scripts/` and
   `.buildkite/` into `.ci-boot`, a few seconds.
2. `remove-stale-build-containers.sh <key>`: removes a container a cancelled
   job left building in this key's checkout.
3. `checkout.sh sync`: creates the key's checkout if the agent has none
   (`CHROMIUM_CHECKOUT_PREPARE=1`), puts it at the commit, then runs
   `gclient sync` and the hooks in the build image.
4. `build-in-container.sh`: runs `node build.js` in the image. That embeds the
   record/replay driver, writes `out/Release/args.gn` to import
   `replay_build_scripts/linux_args.gn`, runs `gn gen` and builds `chrome`.
5. `package-artifacts.sh`: archives the browser as `<build id>.tar.xz`.

The artifacts are `build_id/linux/x86_64/build_id`,
`build_id/linux/x86_64/<build id>.tar.xz`, and `logs/build.log.gz` with
`logs/failed-steps.txt` (every failed step's output, when there are any).

## How long it takes

A cold build compiles all of Chromium on the agent's 8 cores, which takes many
hours. 108 takes minutes because its compiles run on EngFlow; 158 has no remote
execution set up (`use_remoteexec = false`). The out dir is kept in the key's
checkout, so the next build only compiles what changed, and a build that timed
out or was cancelled is continued by the next one.

`REPLAY_NINJA_KEEP_GOING=0` makes one build report every compile error rather
than stop at the first. The build container runs at a low CPU share, so other
work on the agent gets the CPU first.

## Differences from upstream's scripts

- **Checkout.** The fork is fetched from github over https, one commit deep.
  A job can prepare the checkout itself. `gclient sync` and the hooks run on
  every job (upstream syncs the four Replay forks by hand because 108 is too old
  for gclient; 158's DEPS pins the forks itself). No backend checkout and no
  shared `.git-cache`.
- **Image.** Ubuntu 22.04 with the packages `build/install-build-deps.py` of
  the 158 tag installs, and a current depot_tools pin. The image's user has the
  uid of whoever built it, because agents differ.
- **Arguments.** `linux_args.gn` is the argument set the 158 port's Linux build
  uses.
- **No EngFlow, no buildevents, no S3.** The agent has none of those secrets
  wired up for this pipeline. `package-artifacts.sh` replaces
  `upload-artifacts.sh`.
- **The driver** is downloaded by `build.js` exactly as upstream does, from
  `https://static.replay.io/downloads/linux-recordreplay.tgz` (public). It is
  embedded in the browser but is the 108-era driver: enough to compile and
  link, not a statement that this browser records with it.

## By hand

```sh
replay_build_scripts/ci/checkout.sh list --sizes   # what is prepared here
replay_build_scripts/ci/checkout.sh key            # the key for this tree: m158
replay_build_scripts/ci/checkout.sh wipe-out m158  # remove the build outputs
replay_build_scripts/ci/checkout.sh wipe m158      # remove all of that key's state
replay_build_scripts/ci/build-in-container.sh -- bash   # a shell in the image
```
