# Homebrew formula for ac3cli, ac3forge's command-line encoder/decoder.
#
# Builds the CLI only (AC3FORGE_BUILD_CLI=ON, everything else the library
# doesn't need for that off) - same reasoning as the vcpkg port
# (packaging/vcpkg-port/ac3forge/) staying library-only, just the other way
# round: Homebrew formulae are for end-user tools, so this ships the tool
# vcpkg deliberately does not, and skips find_package(ac3forge) dev files
# vcpkg already covers. The Qt6 GUI (ac3gui) is not packaged here - a Homebrew
# Cask, not a Formula, is the right shape for a bundled .app, and needs its
# own follow-up.
#
# Staged here (packaging/homebrew/Formula/ac3forge.rb) for local
# `brew install --build-from-source` validation against this repo before
# being copied into a personal tap (e.g. homebrew-ac3forge) as
# Formula/ac3forge.rb - see docs/releasing.md.
class Ac3forge < Formula
  desc "Clean-room AC-3/E-AC-3 encoder, decoder and Atmos object-layer CLI"
  homepage "https://github.com/iainchesworthlabs/ac3forge"
  url "https://github.com/iainchesworthlabs/ac3forge/archive/refs/tags/v0.8.0-beta.1.tar.gz"
  # Computed directly (sha256sum) from the same release tarball the vcpkg
  # port's portfile.cmake pins by SHA512 - see that file's comment. If
  # `brew install` reports a mismatch, trust brew's reported hash over this
  # one and update it here.
  sha256 "69da9af8c2afe1b3eb37695378219d45391779d69fc2b44b2353935c769c09ee"
  license "GPL-3.0-or-later"
  head "https://github.com/iainchesworthlabs/ac3forge.git", branch: "main"

  depends_on "cmake" => :build

  def install
    # DERIVED_VERSION_OVERRIDE: cmake/GitVersionDerivation.cmake derives the
    # project version via `git describe`, which finds nothing in a release
    # tarball (no .git directory) and silently falls back to "0.0.0-dev".
    # `version` here is Homebrew's own parse of the url= tag, so re-adding
    # the "v" prefix recovers the real tag - same technique
    # packaging/vcpkg-port/ac3forge/portfile.cmake uses for the same reason.
    system "cmake", "-S", ".", "-B", "build",
                     "-DAC3FORGE_BUILD_CLI=ON",
                     "-DAC3FORGE_BUILD_GUI=OFF",
                     "-DAC3FORGE_BUILD_TESTS=OFF",
                     "-DAC3FORGE_BUILD_EXAMPLES=OFF",
                     "-DAC3FORGE_BUILD_FUZZERS=OFF",
                     "-DAC3FORGE_FETCH_CATCH2=OFF",
                     "-DDERIVED_VERSION_OVERRIDE=v#{version}",
                     *std_cmake_args
    system "cmake", "--build", "build"
    system "cmake", "--install", "build"
  end

  test do
    assert_match "ac3forge #{version}", shell_output("#{bin}/ac3cli --version")
  end
end
