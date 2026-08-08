#------------------------------------------------------------------------------
# Windows MSVC Environment Bootstrap
#
# Shared by windows.msvc.toolchain.cmake and windows.llvm.toolchain.cmake.
#
# Both need the MSVC build environment: cl.exe and link.exe need INCLUDE/LIB,
# and clang-cl links against the same CRT and Windows SDK. That environment
# normally only exists inside a "Developer PowerShell for VS". Outside one,
# cl.exe is not on PATH at all while clang.exe often is - which is exactly how
# a Ninja configure used to silently pick a compiler nobody asked for.
#
# So: if the developer environment is absent, find the VS install with vswhere
# and import vcvarsall's environment into this CMake process. Child processes
# (the compiler checks, try_compile, ninja) inherit it, so a preset configures
# identically from a plain shell and from a Developer PowerShell.
#
# Sets AC3_MSVC_TOOLS_DIR: VCToolsInstallDir with the trailing separator removed
# and forward slashes, ready for a subdirectory to be appended.
#------------------------------------------------------------------------------

# Both VCToolsInstallDir and INCLUDE are required to consider the environment
# usable: vcvarsall sets both, so one without the other means a half-configured
# shell we should not trust. This is also what makes the import run only once
# per CMake process - and what lets try_compile's child CMake skip it, since it
# inherits the environment we already built.
if(NOT (DEFINED ENV{VCToolsInstallDir} AND DEFINED ENV{INCLUDE}))

    find_program(AC3_VSWHERE_EXECUTABLE
        NAMES vswhere.exe
        PATHS
            "$ENV{ProgramFiles\(x86\)}/Microsoft Visual Studio/Installer"
            "$ENV{ProgramFiles}/Microsoft Visual Studio/Installer"
            "C:/Program Files (x86)/Microsoft Visual Studio/Installer"
        NO_DEFAULT_PATH)

    if(NOT AC3_VSWHERE_EXECUTABLE)
        message(FATAL_ERROR
            "No MSVC developer environment and vswhere.exe was not found, so one "
            "cannot be created. Install the Visual Studio Build Tools, or configure "
            "from a Developer PowerShell for VS.")
    endif()

    # -products * so Build Tools installs (which are not "Community" or
    # "Professional") are considered; -requires pins the x64 native toolset
    # rather than any VS that happens to have only the .NET workload.
    execute_process(
        COMMAND "${AC3_VSWHERE_EXECUTABLE}"
                -latest
                -products *
                -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64
                -property installationPath
        OUTPUT_VARIABLE _ac3_vs_install
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)

    if(NOT _ac3_vs_install)
        message(FATAL_ERROR
            "vswhere found no Visual Studio install carrying the x64 MSVC toolset "
            "(Microsoft.VisualStudio.Component.VC.Tools.x86.x64).")
    endif()

    set(_ac3_vcvarsall "${_ac3_vs_install}/VC/Auxiliary/Build/vcvarsall.bat")
    if(NOT EXISTS "${_ac3_vcvarsall}")
        message(FATAL_ERROR "Expected vcvarsall.bat at ${_ac3_vcvarsall}, but it is not there.")
    endif()

    # Run vcvarsall through a generated .bat rather than passing the command
    # inline. cmd.exe does not understand the backslash-escaped quotes CMake
    # emits when it quotes an argument containing spaces, so an inline
    # `call "..." x64 && set` breaks the moment the VS install path contains a
    # space - which it always does.
    set(_ac3_vcvars_bat "${CMAKE_BINARY_DIR}/ac3-vcvars.bat")
    file(WRITE "${_ac3_vcvars_bat}" "@echo off\r\ncall \"${_ac3_vcvarsall}\" x64 >NUL\r\nset\r\n")

    execute_process(
        COMMAND "$ENV{COMSPEC}" /c "${_ac3_vcvars_bat}"
        OUTPUT_VARIABLE _ac3_vcvars_env
        RESULT_VARIABLE _ac3_vcvars_result
        ERROR_VARIABLE _ac3_vcvars_error)

    if(NOT _ac3_vcvars_result EQUAL 0)
        message(FATAL_ERROR "vcvarsall.bat x64 failed (${_ac3_vcvars_result}): ${_ac3_vcvars_error}")
    endif()

    # Walk the `set` dump one line at a time, peeling the head off the remainder.
    #
    # Deliberately NOT a foreach over a CMake list. Turning the dump into a list
    # makes every ";" in it a separator, so PATH/INCLUDE/LIB have to be escaped
    # first - and escaping them is not enough, because Windows directory values
    # routinely end in a backslash (VCToolsInstallDir does). That trailing
    # backslash escapes the very separator the split relies on, silently welding
    # the next variable onto the end of the previous one's value.
    set(_ac3_rest "${_ac3_vcvars_env}")
    set(_ac3_imported 0)

    while(_ac3_rest MATCHES "^([^\r\n]*)\r?\n(.*)$")
        set(_ac3_line "${CMAKE_MATCH_1}")
        set(_ac3_rest "${CMAKE_MATCH_2}")

        # Requiring a leading letter or underscore skips cmd's internal
        # pseudo-variables ("=C:=C:\...", "=ExitCode=..."), which are not real
        # environment entries.
        if(_ac3_line MATCHES "^([A-Za-z_][A-Za-z0-9_()]*)=(.*)$")
            # Copy the captures out before the next if(): every MATCHES resets
            # CMAKE_MATCH_<n>, so reading them after the VCPKG_ test below would
            # read that test's captures instead of this one's.
            set(_ac3_name "${CMAKE_MATCH_1}")
            set(_ac3_value "${CMAKE_MATCH_2}")

            # Everything except VCPKG_*. vcvarsall exports VCPKG_ROOT pointing at
            # the cut-down vcpkg bundled with Visual Studio, which is not a git
            # checkout and refuses manifest work without a builtin-baseline. We
            # are here to find a compiler; which package manager the developer
            # chose is not vcvarsall's to decide.
            if(NOT _ac3_name MATCHES "^VCPKG_")
                set(ENV{${_ac3_name}} "${_ac3_value}")
                math(EXPR _ac3_imported "${_ac3_imported} + 1")
            endif()
        endif()
    endwhile()

    if(NOT DEFINED ENV{VCToolsInstallDir})
        message(FATAL_ERROR
            "vcvarsall.bat ran but did not set VCToolsInstallDir; the imported "
            "environment is not usable.")
    endif()

    message(STATUS "Imported MSVC environment from ${_ac3_vs_install} (${_ac3_imported} variables)")

    unset(_ac3_vs_install)
    unset(_ac3_vcvarsall)
    unset(_ac3_vcvars_bat)
    unset(_ac3_vcvars_env)
    unset(_ac3_vcvars_result)
    unset(_ac3_vcvars_error)
    unset(_ac3_rest)
    unset(_ac3_line)
    unset(_ac3_name)
    unset(_ac3_value)
    unset(_ac3_imported)

endif()

# vcvarsall leaves a trailing backslash on its directory variables, which turns
# into "...\/bin/Hostx64/x64" the moment a subdirectory is appended. Normalise
# once here so every consumer can just append.
set(AC3_MSVC_TOOLS_DIR "$ENV{VCToolsInstallDir}")
string(REGEX REPLACE "[\\\\/]+$" "" AC3_MSVC_TOOLS_DIR "${AC3_MSVC_TOOLS_DIR}")
file(TO_CMAKE_PATH "${AC3_MSVC_TOOLS_DIR}" AC3_MSVC_TOOLS_DIR)

#------------------------------------------------------------------------------
# Bake the CRT and Windows SDK search paths into the generated build rules
# instead of leaving them in INCLUDE and LIB.
#
# The environment above only exists inside the *configure* process. `cmake
# --build` runs ninja from whatever shell the developer is in, and ninja invokes
# cl.exe with that shell's environment - so an INCLUDE that only configure knows
# about produces a configure that succeeds and a build that cannot find
# <algorithm>. Carrying the paths on the compile and link lines makes the build
# tree self-contained: it no longer matters which shell drives it, and the same
# build.ninja means the same command whoever runs it.
#
# TO_CMAKE_PATH is the documented way to turn a native ';'-separated search path
# into a CMake list; it rewrites the separators first, so the trailing
# backslashes MSVC leaves on these entries never get a chance to escape one.
#------------------------------------------------------------------------------
file(TO_CMAKE_PATH "$ENV{INCLUDE}" AC3_MSVC_INCLUDE_DIRS)
file(TO_CMAKE_PATH "$ENV{LIB}" AC3_MSVC_LIBRARY_DIRS)

set(CMAKE_C_STANDARD_INCLUDE_DIRECTORIES ${AC3_MSVC_INCLUDE_DIRS})
set(CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES ${AC3_MSVC_INCLUDE_DIRS})
set(CMAKE_RC_STANDARD_INCLUDE_DIRECTORIES ${AC3_MSVC_INCLUDE_DIRS})

set(_ac3_libpath "")
foreach(_ac3_dir IN LISTS AC3_MSVC_LIBRARY_DIRS)
    string(APPEND _ac3_libpath " /LIBPATH:\"${_ac3_dir}\"")
endforeach()

set(CMAKE_EXE_LINKER_FLAGS_INIT "${_ac3_libpath}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_ac3_libpath}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_ac3_libpath}")

unset(_ac3_libpath)
unset(_ac3_dir)

message(STATUS "MSVC toolset: ${AC3_MSVC_TOOLS_DIR}")
