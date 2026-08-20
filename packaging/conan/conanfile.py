# Conan (2.x) recipe for ac3forge - installs the library only (ac3::forge,
# plus matroska::matroska, mp4::mp4 and mpegts::mpegts behind their own
# default-on options), never the CLI, GUI, tests, examples or fuzz
# harnesses. Same scope as the vcpkg port (packaging/vcpkg-port/ac3forge/) -
# one Conan option <-> one AC3FORGE_BUILD_<NAME> CMake option, same pattern
# that port's vcpkg_check_features() call already establishes. ac3adm::ac3adm
# (the ADM/BW64 reader) is deliberately NOT an option here for the same
# reason it has no vcpkg feature - it isn't part of the find_package(ac3forge)
# package at all, so there's nothing for a Conan option to install.
#
# This recipe wraps cmake/InstallLibrary.cmake's own install()/export()
# rules rather than reimplementing them: package() just runs `cmake --install`
# and package_info() points consumers at the CMake package config ac3forge
# already generates (ac3forgeConfig.cmake et al.), instead of asking Conan's
# CMakeDeps generator to synthesise a second, competing one - see
# package_info()'s comment below.
#
# Staged here (packaging/conan/) for local `conan create` validation before
# being submitted to ConanCenter (conan-center-index) as a recipe there -
# see docs/releasing.md.
import os

from conan import ConanFile
from conan.tools.build import check_min_cppstd
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout
from conan.tools.files import copy, get


class Ac3forgeConan(ConanFile):
    name = "ac3forge"
    description = (
        "Clean-room AC-3 (ATSC A/52) encoder and decoder with a spatial "
        "object layer, in C++23."
    )
    license = "GPL-3.0-or-later"
    homepage = "https://github.com/iainchesworthlabs/ac3forge"
    url = "https://github.com/iainchesworthlabs/ac3forge"
    topics = ("audio", "codec", "ac3", "dolby-digital", "atmos", "eac3")
    package_type = "library"

    settings = "os", "arch", "compiler", "build_type"
    options = {
        "shared": [True, False],
        "fPIC": [True, False],
        "matroska": [True, False],
        "mp4": [True, False],
        "mpegts": [True, False],
    }
    default_options = {
        "shared": False,
        "fPIC": True,
        # Matches packaging/vcpkg-port/ac3forge/vcpkg.json's default-features:
        # all three container writers on by default.
        "matroska": True,
        "mp4": True,
        "mpegts": True,
    }

    def config_options(self):
        if self.settings.os == "Windows":
            self.options.rm_safe("fPIC")

    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")

    def layout(self):
        cmake_layout(self)

    def validate(self):
        check_min_cppstd(self, 23)

    def source(self):
        get(self, **self.conan_data["sources"][self.version], strip_root=True)

    def generate(self):
        tc = CMakeToolchain(self)
        # Library only - same OFF set as portfile.cmake's
        # vcpkg_cmake_configure() call.
        tc.variables["AC3FORGE_BUILD_CLI"] = False
        tc.variables["AC3FORGE_BUILD_GUI"] = False
        tc.variables["AC3FORGE_BUILD_TESTS"] = False
        tc.variables["AC3FORGE_BUILD_EXAMPLES"] = False
        tc.variables["AC3FORGE_BUILD_FUZZERS"] = False
        tc.variables["AC3FORGE_FETCH_CATCH2"] = False
        # A Conan package (like a vcpkg triplet) installs exactly the
        # linkage this recipe's own `shared` option/BUILD_SHARED_LIBS
        # selected, not both - see cmake/InstallLibrary.cmake's option of
        # the same name.
        tc.variables["AC3FORGE_INSTALL_BOTH_LINKAGES"] = False
        tc.variables["AC3FORGE_BUILD_MATROSKA"] = bool(self.options.matroska)
        tc.variables["AC3FORGE_BUILD_MP4"] = bool(self.options.mp4)
        tc.variables["AC3FORGE_BUILD_MPEGTS"] = bool(self.options.mpegts)
        tc.variables["BUILD_SHARED_LIBS"] = bool(self.options.shared)
        tc.generate()

    def build(self):
        cmake = CMake(self)
        # DERIVED_VERSION_OVERRIDE has to reach the initial `cmake` command
        # line, not just the generated toolchain file: root CMakeLists.txt's
        # include(GitVersionDerivation.cmake) runs before the first
        # project()/enable_language() call, which is the point at which
        # CMAKE_TOOLCHAIN_FILE - and so any CACHE variable a tc.variables[...]
        # entry would have written into it - actually gets loaded. A plain
        # cli_args -D, like vcpkg_cmake_configure()'s OPTIONS in
        # portfile.cmake, is visible immediately instead. Confirmed by
        # running the toolchain-file version first: it silently fell back to
        # "0.0.0-dev" in the installed ac3/version.hpp.
        cmake.configure(cli_args=[f"-DDERIVED_VERSION_OVERRIDE=v{self.version}"])
        cmake.build()

    def package(self):
        copy(self, "LICENSE", self.source_folder, os.path.join(self.package_folder, "licenses"))
        cmake = CMake(self)
        cmake.install()

    def package_info(self):
        # ac3forge exports its own CMake package config
        # (cmake/InstallLibrary.cmake's configure_package_config_file() +
        # install(EXPORT ...) calls - ac3forgeConfig.cmake,
        # forgeTargets.cmake, and one *Targets.cmake per enabled
        # component) rather than relying on Conan's CMakeDeps generator to
        # synthesise one. cmake_find_mode "none" tells CMakeDeps to stay out
        # of the way; builddirs puts the package's own installed config on
        # CMAKE_PREFIX_PATH so a consumer's plain
        # find_package(ac3forge CONFIG REQUIRED) resolves it directly -
        # same find_package() call and ac3::forge/matroska::matroska/
        # mp4::mp4/mpegts::mpegts targets as any other consumer in
        # docs/library/index.md, Conan or not.
        self.cpp_info.set_property("cmake_find_mode", "none")
        self.cpp_info.builddirs = [os.path.join("lib", "cmake", "ac3forge")]
