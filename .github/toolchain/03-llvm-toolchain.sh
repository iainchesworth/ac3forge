#!/usr/bin/env bash
set -euo pipefail
#
# Copied verbatim from aqualink-automate
# (cicd/packer/scripts/linux/03-llvm-toolchain.sh) so the two projects pin the same
# toolchain. Consumed by .github/workflows/_build.yml, which runs it with sudo
# on the GitHub-hosted Ubuntu runner. Verified against Ubuntu 26.04 LTS, where
# the distro archive carries both compilers (gcc 15.2.0, clang 21.1.8) and
# neither fallback path is taken.
#

# LLVM/Clang major version the project pins to. Keep this in step with the
# toolchain files (cmake/toolchains/*.llvm.toolchain.cmake) and the vcpkg triplets.
LLVM_VERSION=21

# apt.llvm.org dist codename for the FALLBACK path only (see below). Defaults to
# the running release; override with the env var if a base needs a different repo.
LLVM_REPO_CODENAME="${LLVM_REPO_CODENAME:-$(lsb_release -cs)}"

echo "==> Installing LLVM/Clang ${LLVM_VERSION} toolchain"

PACKAGES=(
    "clang-${LLVM_VERSION}"
    "clang-tidy-${LLVM_VERSION}"
    "lld-${LLVM_VERSION}"
    "libc++-${LLVM_VERSION}-dev"
    "libc++abi-${LLVM_VERSION}-dev"
    # ASan/UBSan's static runtime libs (libclang_rt.*.a) - without this package
    # any -fsanitize= link fails with "cannot find libclang_rt.*.a". Not needed
    # by a plain build, but every Linux LLVM leg shares this script, so it is
    # simplest to always have it rather than fork the install for the
    # sanitizer presets alone.
    "libclang-rt-${LLVM_VERSION}-dev"
    # llvm-symbolizer, which ASan/UBSan shell out to at runtime to turn
    # addresses back into file:line - without it a report is a stack of raw
    # addresses. It ships in llvm-${LLVM_VERSION}, not clang-${LLVM_VERSION}.
    "llvm-${LLVM_VERSION}"
)

# Ubuntu 26.04 LTS (resolute) and newer ship this exact set in the 'universe'
# component (clang-${LLVM_VERSION} = 1:21.1.8…, pool/universe/l/llvm-toolchain-21).
# Installing from the distribution archive means the build does NOT depend on
# apt.llvm.org, which has had sustained (~30 min) pool outages that used to block
# the whole template build. Ensure 'universe' is enabled — it already is on a
# stock Ubuntu Server install; this is an idempotent safety net for trimmed bases.
add-apt-repository -y universe >/dev/null 2>&1 || true
apt-get update

if apt-cache show "clang-${LLVM_VERSION}" >/dev/null 2>&1; then
    echo "==> Installing LLVM ${LLVM_VERSION} from the distribution archive (no apt.llvm.org)"
else
    # Fallback ONLY for bases whose archive predates LLVM ${LLVM_VERSION} (e.g. an
    # older LTS): pull from apt.llvm.org. This path keeps the historical hardening
    # — retries, a key fetch, and a per-version repo probe that auto-falls back to
    # the nearest LTS (noble) when the running codename has no LLVM repo — because
    # apt.llvm.org is the only source left when the distro doesn't carry clang.
    echo "==> Distro archive has no clang-${LLVM_VERSION}; falling back to apt.llvm.org"

    # Add the LLVM apt signing key (retry — apt.llvm.org is frequently flaky and a
    # single transient error here used to abort the whole build).
    curl -fsSL --retry 5 --retry-all-errors --retry-delay 3 --max-time 60 \
        https://apt.llvm.org/llvm-snapshot.gpg.key \
        | gpg --dearmor -o /usr/share/keyrings/llvm-archive-keyring.gpg

    # Probe the ACTUAL per-version repo, with retries. The bare /<codename>/ URL
    # returns 200 even for releases that have no LLVM repo (a false signal the old
    # check relied on), so probe the real dist Release file instead.
    repo_has_llvm() {
        curl -fsS --retry 5 --retry-all-errors --retry-delay 3 --max-time 30 --head \
            "https://apt.llvm.org/$1/dists/llvm-toolchain-$1-${LLVM_VERSION}/Release" \
            >/dev/null 2>&1
    }

    CODENAME="${LLVM_REPO_CODENAME}"
    if ! repo_has_llvm "$CODENAME"; then
        echo "WARNING: no LLVM ${LLVM_VERSION} repo for '${CODENAME}', falling back to 'noble'"
        CODENAME="noble"
        if ! repo_has_llvm "$CODENAME"; then
            echo "ERROR: no LLVM ${LLVM_VERSION} repo for '${CODENAME}' either; see https://apt.llvm.org/"
            exit 1
        fi
    fi

    echo "deb [signed-by=/usr/share/keyrings/llvm-archive-keyring.gpg] https://apt.llvm.org/${CODENAME}/ llvm-toolchain-${CODENAME}-${LLVM_VERSION} main" \
        > /etc/apt/sources.list.d/llvm.list

    # apt-get retries are configured globally in 01-base-packages.sh
    # (/etc/apt/apt.conf.d/80-retries), so these survive transient apt.llvm.org errors.
    apt-get update
fi

apt-get install -y --no-install-recommends "${PACKAGES[@]}"

# Set up alternatives
update-alternatives --install /usr/bin/clang clang "/usr/bin/clang-${LLVM_VERSION}" 100
update-alternatives --install /usr/bin/clang++ clang++ "/usr/bin/clang++-${LLVM_VERSION}" 100
update-alternatives --install /usr/bin/clang-tidy clang-tidy "/usr/bin/clang-tidy-${LLVM_VERSION}" 100
update-alternatives --install /usr/bin/lld lld "/usr/bin/lld-${LLVM_VERSION}" 100
update-alternatives --install /usr/bin/llvm-symbolizer llvm-symbolizer "/usr/bin/llvm-symbolizer-${LLVM_VERSION}" 100

echo "==> Clang $(clang --version | head -1) installed"

# Guard against a silent major-version drift (e.g. the distro moving clang to 22).
clang --version | grep -qE "version ${LLVM_VERSION}\." \
    || { echo "ERROR: clang is not version ${LLVM_VERSION}"; exit 1; }
