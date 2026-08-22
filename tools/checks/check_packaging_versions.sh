#!/usr/bin/env bash
#
# Packaging manifest consistency guard.
#
# This does NOT check "does packaging/ point at the latest release tag" -
# docs/releasing.md's post-release checklist documents the four packaging
# bumps as their own follow-up step, done asynchronously after a release
# goes out (sometimes days later, in a separate PR), not atomically with the
# tag. A "must match the latest tag" gate would therefore fail by design in
# the normal gap between tagging and getting around to the bump PRs - noise,
# not signal.
#
# What this DOES catch: internal inconsistency within a manifest, the kind a
# manual copy-forward-and-edit bump (see docs/releasing.md's per-ecosystem
# steps) can introduce - a version string updated in one file of a set but
# not another, a hash of the wrong length, a URL that does not name the
# version it is filed under.
#
# Usage:  ./tools/checks/check_packaging_versions.sh [-r <repo-root>]
# Exit:   0 = clean, 1 = inconsistency found.

set -euo pipefail

root="."
while getopts "r:" opt; do
    case "$opt" in
        r) root="$OPTARG" ;;
        *) echo "Usage: $0 [-r <repo-root>]" >&2; exit 2 ;;
    esac
done

fail=0
note() { echo "::error::$1"; echo "  $1"; fail=1; }

# --- winget: every version directory's three files must agree with each
# other and with the directory name, and the installer's hash/URL must be
# well-formed. ---
winget_root="$root/packaging/winget/manifests/i/iainchesworthlabs/ac3forge"
if [ -d "$winget_root" ]; then
    for dir in "$winget_root"/*/; do
        version="$(basename "$dir")"
        installer="$dir/iainchesworthlabs.ac3forge.installer.yaml"
        locale="$dir/iainchesworthlabs.ac3forge.locale.en-US.yaml"
        manifest="$dir/iainchesworthlabs.ac3forge.yaml"

        for f in "$installer" "$locale" "$manifest"; do
            [ -f "$f" ] || note "winget $version: missing $(basename "$f")"
        done
        [ -f "$installer" ] && [ -f "$locale" ] && [ -f "$manifest" ] || continue

        for f in "$installer" "$locale" "$manifest"; do
            pv="$(grep -m1 '^PackageVersion:' "$f" | sed 's/^PackageVersion:[[:space:]]*//')"
            if [ "$pv" != "$version" ]; then
                note "winget $version: $(basename "$f") has PackageVersion: $pv, expected $version"
            fi
        done

        url="$(grep -m1 'InstallerUrl:' "$installer" | sed 's/^[[:space:]]*InstallerUrl:[[:space:]]*//')"
        case "$url" in
            *"/v$version/"*) ;;
            *) note "winget $version: InstallerUrl does not reference v$version ($url)" ;;
        esac

        sha="$(grep -m1 'InstallerSha256:' "$installer" | sed 's/^[[:space:]]*InstallerSha256:[[:space:]]*//')"
        if ! echo "$sha" | grep -qE '^[0-9A-Fa-f]{64}$'; then
            note "winget $version: InstallerSha256 is not 64 hex characters ($sha)"
        fi
    done
else
    note "winget manifest root not found: $winget_root"
fi

# --- conan: every sources: entry's key, url and sha256 must agree with each
# other. ---
conandata="$root/packaging/conan/conandata.yml"
if [ -f "$conandata" ]; then
    # Each version block is "  \"X.Y.Z...\":" followed by indented url:/sha256: lines.
    version=""
    while IFS= read -r line; do
        case "$line" in
            '  "'*'":')
                version="$(echo "$line" | sed -E 's/^  "([^"]+)":.*/\1/')"
                ;;
            *url:*)
                url="$(echo "$line" | sed -E 's/^[[:space:]]*url:[[:space:]]*"?([^"]*)"?[[:space:]]*$/\1/')"
                case "$url" in
                    *"/v$version.tar.gz") ;;
                    *) note "conan $version: url does not reference v$version.tar.gz ($url)" ;;
                esac
                ;;
            *sha256:*)
                sha="$(echo "$line" | sed -E 's/^[[:space:]]*sha256:[[:space:]]*"?([^"]*)"?[[:space:]]*$/\1/')"
                if ! echo "$sha" | grep -qE '^[0-9A-Fa-f]{64}$'; then
                    note "conan $version: sha256 is not 64 hex characters ($sha)"
                fi
                ;;
        esac
    done < "$conandata"
else
    note "conan manifest not found: $conandata"
fi

# --- homebrew: the Formula and Cask should agree on which release they pin,
# even though they version independently (Formula from source, Cask from a
# prebuilt .dmg) - see docs/releasing.md#homebrew-formula-and-cask. ---
formula="$root/packaging/homebrew/Formula/ac3forge.rb"
cask="$root/packaging/homebrew/Casks/ac3gui.rb"
if [ -f "$formula" ] && [ -f "$cask" ]; then
    formula_version="$(grep -m1 -oE 'archive/refs/tags/v[0-9][^"'"'"']*' "$formula" | sed -E 's#archive/refs/tags/v##; s/\.tar\.gz$//')"
    cask_version="$(grep -m1 -oE '^\s*version\s+"[^"]+"' "$cask" | sed -E 's/^\s*version\s+"([^"]+)"/\1/')"
    if [ -n "$formula_version" ] && [ -n "$cask_version" ] && [ "$formula_version" != "$cask_version" ]; then
        note "homebrew: Formula pins v$formula_version but Cask pins v$cask_version"
    fi
else
    note "homebrew formula/cask not found under $root/packaging/homebrew"
fi

# --- vcpkg port: portfile's SHA512 must be well-formed. ---
portfile="$root/packaging/vcpkg-port/ac3forge/portfile.cmake"
if [ -f "$portfile" ]; then
    sha="$(grep -m1 -oE 'SHA512 [0-9A-Fa-f]+' "$portfile" | awk '{print $2}')"
    if ! echo "$sha" | grep -qE '^[0-9A-Fa-f]{128}$'; then
        note "vcpkg port: SHA512 is not 128 hex characters ($sha)"
    fi
else
    note "vcpkg portfile not found: $portfile"
fi

if [ "$fail" -ne 0 ]; then
    echo ""
    echo "Packaging manifest inconsistency found - see docs/releasing.md for the per-ecosystem update steps."
    exit 1
fi

echo "OK: packaging manifests are internally consistent."
