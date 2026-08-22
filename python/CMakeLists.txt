# ---------------------------------------------------------------------------
# The pybind11 extension backing the `ac3forge` PyPI package (roadmap F2).
# Bound straight onto ac3::forge's existing C++ classes (pybind11-direct, per
# the roadmap's own dependency note) - no intermediate C API, unlike a normal
# third-party-language binding, because pybind11 talks C++ ABI directly.
#
# Included from the root CMakeLists.txt only when AC3FORGE_BUILD_PYTHON is ON
# (default OFF, see that option's own comment) - normally driven by
# python/pyproject.toml's cmake.define when scikit-build-core configures this
# tree for `pip install`/cibuildwheel, not by a developer's ordinary build.
# ---------------------------------------------------------------------------

find_package(pybind11 CONFIG REQUIRED)

pybind11_add_module(_ac3forge src/ac3forge_ext/bindings.cpp)

# Deliberately not ac3::warnings, unlike every other in-tree consumer of
# ac3::forge (examples/, tests/, apps/cli, apps/gui) - that target makes this
# project's OWN warnings fatal, and pybind11's vendored headers are not this
# project's code to hold to that bar. A real external consumer wrapping
# ac3::forge would not inherit ac3::warnings either; this target is exactly
# that kind of consumer, just an in-tree one.
target_link_libraries(_ac3forge PRIVATE ac3::forge)

# scikit-build-core's wheel.install-dir (see python/pyproject.toml) is the
# package root, so a relative DESTINATION here lands the compiled extension
# next to python/src/ac3forge/__init__.py inside the wheel.
#
# COMPONENT python, paired with pyproject.toml's install.components = ["python"]: the root
# CMakeLists.txt's include(InstallLibrary) always installs the full ac3::forge SDK (headers,
# .lib/.dll, CMake package config) under COMPONENT library/libruntime, unconditionally - it has
# no idea this configure is a Python build and shouldn't skip its normal job for every OTHER
# consumer (the windows-msvc CI leg, a real `cmake --install`/CPack release). Restricting
# scikit-build-core to installing only this one component is what keeps that SDK content out of
# the wheel - confirmed the hard way: without this, the wheel had `include/ac3/*.hpp`,
# `lib/ac3forge*.lib`, `bin/ac3forge.dll` and `lib/cmake/ac3forge/*.cmake` sitting loose in
# site-packages alongside the actual `ac3forge/_ac3forge*.pyd` extension.
install(TARGETS _ac3forge DESTINATION ac3forge COMPONENT python)
