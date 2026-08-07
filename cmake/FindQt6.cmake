# ---------------------------------------------------------------------------
# FindQt6.cmake - wrapper picked up automatically by find_package(Qt6 ...)
# because cmake/ is on CMAKE_MODULE_PATH (CMake tries Module mode, i.e. a
# Find<Pkg>.cmake, before Config mode).
#
# It never replaces Qt6Config.cmake: it only widens CMAKE_PREFIX_PATH with the
# standard prebuilt-Qt install layout (C:/Qt, ~/Qt, /opt/Qt, ...) when Qt6
# isn't already discoverable, then defers to Qt's own config package for the
# real work. An explicit -DCMAKE_PREFIX_PATH=... or -DQt6_DIR=... always wins.
#
# Qt is deliberately NOT a vcpkg dependency here - building it from source
# costs hours and gigabytes for no benefit over the official prebuilt kits.
# ---------------------------------------------------------------------------

# Respect anything the caller already pointed CMake at.
find_package(Qt6 ${Qt6_FIND_VERSION} CONFIG QUIET COMPONENTS ${Qt6_FIND_COMPONENTS})

if(NOT Qt6_FOUND)
    if(WIN32)
        set(_qt6_search_roots "C:/Qt" "$ENV{USERPROFILE}/Qt" "D:/Qt")
        set(_qt6_arch_dirs "msvc2022_64" "msvc2019_64" "mingw_64")
    elseif(APPLE)
        set(_qt6_search_roots "$ENV{HOME}/Qt" "/opt/Qt" "/usr/local/opt/qt6" "/opt/homebrew/opt/qt6")
        set(_qt6_arch_dirs "macos" "clang_64")
    else()
        set(_qt6_search_roots "$ENV{HOME}/Qt" "/opt/Qt" "/usr/lib/qt6" "/usr/lib/x86_64-linux-gnu/qt6")
        set(_qt6_arch_dirs "gcc_64")
    endif()

    set(_qt6_candidates "")
    foreach(root IN LISTS _qt6_search_roots)
        if(NOT root OR NOT IS_DIRECTORY "${root}")
            continue()
        endif()

        file(GLOB _qt6_version_dirs LIST_DIRECTORIES true "${root}/6.*")
        foreach(version_dir IN LISTS _qt6_version_dirs)
            if(NOT IS_DIRECTORY "${version_dir}")
                continue()
            endif()
            foreach(arch IN LISTS _qt6_arch_dirs)
                if(EXISTS "${version_dir}/${arch}/lib/cmake/Qt6/Qt6Config.cmake")
                    list(APPEND _qt6_candidates "${version_dir}/${arch}")
                endif()
            endforeach()
        endforeach()

        # System-wide Qt6 installs (e.g. apt's qt6-base-dev) ship directly under root.
        if(EXISTS "${root}/lib/cmake/Qt6/Qt6Config.cmake")
            list(APPEND _qt6_candidates "${root}")
        endif()
    endforeach()

    if(_qt6_candidates)
        list(SORT _qt6_candidates COMPARE NATURAL)
        list(REVERSE _qt6_candidates)
        list(GET _qt6_candidates 0 _qt6_chosen)

        message(STATUS "FindQt6: auto-detected prebuilt Qt6 at ${_qt6_chosen} (pass -DCMAKE_PREFIX_PATH to override)")
        list(APPEND CMAKE_PREFIX_PATH "${_qt6_chosen}")

        set(_qt6_args CONFIG)
        if(Qt6_FIND_QUIETLY)
            list(APPEND _qt6_args QUIET)
        endif()
        if(Qt6_FIND_REQUIRED)
            list(APPEND _qt6_args REQUIRED)
        endif()
        if(Qt6_FIND_COMPONENTS)
            list(APPEND _qt6_args COMPONENTS ${Qt6_FIND_COMPONENTS})
        endif()

        find_package(Qt6 ${Qt6_FIND_VERSION} ${_qt6_args})

        unset(_qt6_args)
        unset(_qt6_chosen)
    endif()

    unset(_qt6_search_roots)
    unset(_qt6_arch_dirs)
    unset(_qt6_candidates)
    unset(_qt6_version_dirs)
endif()
