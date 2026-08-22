#!/usr/bin/env bash
set -euo pipefail
#
# Copied verbatim from aqualink-automate
# (cicd/packer/scripts/linux/02-gcc-toolchain.sh) so the two projects pin the same
# toolchain — if you change the fallback logic here, port it back there too.
# Consumed by .github/workflows/_build.yml, which runs it with sudo on the
# GitHub-hosted *and* self-hosted Ubuntu runner (both land on the same
# ubuntu:26.04 container image; nothing is baked into a runner image, see
# docs/ci-self-hosted-runners.md).
#

GCC_VERSION=16

echo "==> Installing GCC ${GCC_VERSION} toolchain"

# Ubuntu 26.04 LTS (Resolute Raccoon) ships gcc-16/g++-16, but in the
# `universe` component rather than `main` (unlike gcc-15, which was in main
# outright) — and the official ubuntu:26.04 container image only enables
# `main` by default, so enable universe first and re-probe before falling
# back to the ubuntu-toolchain-r PPA, which exists for an older base whose
# own archive (main or universe) caps out below GCC 16 entirely.
if ! apt-cache show "gcc-${GCC_VERSION}" >/dev/null 2>&1; then
    echo "==> gcc-${GCC_VERSION} not in default repos, enabling the universe component"
    add-apt-repository -y universe
    apt-get update
fi

if ! apt-cache show "gcc-${GCC_VERSION}" >/dev/null 2>&1; then
    echo "==> gcc-${GCC_VERSION} still not found, adding ubuntu-toolchain-r PPA"
    add-apt-repository -y ppa:ubuntu-toolchain-r/test
fi

apt-get update
apt-get install -y --no-install-recommends \
    "gcc-${GCC_VERSION}" \
    "g++-${GCC_VERSION}"

# Set up alternatives so gcc/g++/gcov all point to the correct version
update-alternatives --install /usr/bin/gcc gcc "/usr/bin/gcc-${GCC_VERSION}" "${GCC_VERSION}0" \
    --slave /usr/bin/g++ g++ "/usr/bin/g++-${GCC_VERSION}" \
    --slave /usr/bin/gcov gcov "/usr/bin/gcov-${GCC_VERSION}"
update-alternatives --set gcc "/usr/bin/gcc-${GCC_VERSION}"

# Verify
echo "==> GCC $(gcc --version | head -1) installed"
echo "==> gcov $(gcov --version | head -1) installed"
gcc --version | head -1 | grep -q "${GCC_VERSION}" || { echo "ERROR: gcc version mismatch"; exit 1; }
gcov --version | head -1 | grep -q "${GCC_VERSION}" || { echo "ERROR: gcov version mismatch"; exit 1; }
