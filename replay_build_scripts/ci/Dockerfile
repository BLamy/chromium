# The image Chromium 158 builds run in, on CI and locally
# (build-in-container.sh).
#
# It is built from the repo by build-image.sh, tagged with a hash of this file,
# so a checkout always builds in the image its own branch describes, and two
# Chromium versions in flight get two images.
#
# This is upstream's image (replayio/chromium, replay_build_scripts/ci) moved
# from Chromium 108 to 158:
#
# - The base is Ubuntu 22.04, not bionic: 158's clang, node and gn are built for
#   a newer glibc than bionic has, and depot_tools needs a newer python.
# - The packages are the ones Chromium's own build/install-build-deps.py asks
#   for, at the 158 tag, instead of a list kept by hand.
# - depot_tools is a current pin: 108's pin was held back for reclient.
#
# What is deliberately NOT here: the compiler. Chromium compiles with the clang
# in third_party/llvm-build and links against build/linux/*-sysroot, both
# gclient-managed inside the checkout, so the toolchain is versioned with the
# tree.
FROM ubuntu:22.04@sha256:74b12b11ffa550fba1929ad4d8d16533c34f6612728469671afba1b03b66881c

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
      git curl python3 python3-pip lsb-release sudo xz-utils file \
      ca-certificates pkg-config build-essential nodejs unzip zip lsof && \
    rm -rf /var/lib/apt/lists/*

# Chromium's own list of what a Linux build machine needs. Fetched at the tag
# this fork is based on, because the image is built before any checkout exists.
ARG CHROMIUM_TAG=158.0.8127.18
RUN curl -fsSL "https://chromium.googlesource.com/chromium/src/+/refs/tags/${CHROMIUM_TAG}/build/install-build-deps.py?format=TEXT" \
      | base64 -d > /tmp/install-build-deps.py && \
    apt-get update && \
    python3 /tmp/install-build-deps.py --no-prompt --no-chromeos-fonts --no-arm --no-nacl && \
    rm -rf /var/lib/apt/lists/* /tmp/install-build-deps.py

# The user builds run as is the one that runs build-image.sh, so that the files
# a build writes into the mounted checkout belong to the agent, whatever its uid
# is (1000 on some agents, 992 on others).
ARG BUILD_UID=1000
ARG BUILD_GID=1000
RUN groupadd -o -g "$BUILD_GID" ubuntu && \
    useradd -o -m -s /bin/bash -g "$BUILD_GID" -u "$BUILD_UID" ubuntu

# depot_tools comes from the image at a pin, not from a clone on the agent that
# nothing in any repo describes. DEPOT_TOOLS_UPDATE=0 keeps it from updating
# itself out of that pin.
ARG DEPOT_TOOLS_REVISION=1afcf0756e47f3a6ee82d7736b9fcda2d57b268e
RUN git init -q /depot_tools && \
    git -C /depot_tools remote add origin https://chromium.googlesource.com/chromium/tools/depot_tools.git && \
    git -C /depot_tools fetch -q --depth 1 origin "$DEPOT_TOOLS_REVISION" && \
    git -C /depot_tools checkout -q FETCH_HEAD && \
    echo "$DEPOT_TOOLS_REVISION" > /depot_tools/.replay-revision && \
    touch /depot_tools/.disable_auto_update && \
    chown -R ubuntu:ubuntu /depot_tools
ENV DEPOT_TOOLS_UPDATE=0
ENV PATH=$PATH:/depot_tools

# depot_tools downloads its own python, cipd and vpython on first use and writes
# them into itself. Doing that here means a build never writes to /depot_tools.
USER ubuntu
RUN cd /depot_tools && ./ensure_bootstrap && ./gclient --version && \
    ./cipd --version && ./vpython3 -c "import sys; print(sys.version)" && \
    test -f /depot_tools/python3_bin_reldir.txt
USER root

# The build mounts a checkout that git may see as owned by someone else.
RUN git config --system --add safe.directory '*'

# build.js refuses to build in an image that is not this file's.
ARG BUILD_IMAGE_TAG
RUN test -n "$BUILD_IMAGE_TAG" && echo "$BUILD_IMAGE_TAG" > /etc/chromium-build-image

USER ubuntu
WORKDIR /chromium/src
