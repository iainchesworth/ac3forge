import os

from conan import ConanFile
from conan.tools.build import can_run
from conan.tools.cmake import CMake, cmake_layout


class Ac3forgeTestConan(ConanFile):
    settings = "os", "arch", "compiler", "build_type"
    # CMakeDeps is required even though the ac3forge recipe sets
    # cmake_find_mode "none" for itself: that setting only skips generating
    # a competing Find/Config file, CMakeDeps still resolves ac3forge_ROOT /
    # CMAKE_PREFIX_PATH so this test's plain find_package(ac3forge) call
    # locates the package's own installed CMake config - see
    # packaging/conan/conanfile.py's package_info() comment.
    generators = ("CMakeToolchain", "CMakeDeps")

    def requirements(self):
        self.requires(self.tested_reference_str)

    def layout(self):
        cmake_layout(self)

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def test(self):
        if can_run(self):
            bin_path = os.path.join(self.cpp.build.bindirs[0], "test_ac3forge")
            self.run(bin_path, env="conanrun")
