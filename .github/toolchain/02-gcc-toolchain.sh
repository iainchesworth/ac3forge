#!/usr/bin/env bash
set -euo pipefail
#
# Copied verbatim from aqualink-automate
# (cicd/packer/scripts/linux/02-gcc-toolchain.sh) so the two projects pin the same
# toolchain. Consumed by .github/workflows/_build.yml, which runs it with sudo
# on the GitHub-hosted Ubuntu runner. Verified against Ubuntu 26.04 LTS, where
# the distro archive carries both compilers (gcc 15.2.0, clang 21.1.8) and
# neither fallback path is taken.
#

GCC_VERSION=15

echo "==> Installing GCC ${GCC_VERSION} toolchain"

# Ubuntu 26.04 LTS (Resolute Raccoon) ships gcc-15/g++-15 in main, so this
# install needs no extra source there. The PPA is only a fallback for an older
# base (e.g. a 24.04 noble rebuild) whose main repos cap out below GCC 15 —
# probe the actual package and add ubuntu-toolchain-r/test only if it's missing.
if ! apt-cache show "gcc-${GCC_VERSION}" >/dev/null 2>&1; then
    echo "==> gcc-${GCC_VERSION} not in default repos, adding ubuntu-toolchain-r PPA"
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
