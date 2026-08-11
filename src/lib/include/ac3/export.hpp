#pragma once

// Hand-written for now: this is step 1 of the library-distribution plan,
// before the static+shared CMake target topology lands. AC3FORGE_EXPORT
// expands to nothing here, matching today's static-only build. Step 2
// replaces this file's *generation* with CMake's generate_export_header()
// once the shared build exists (real __declspec(dllexport/dllimport) /
// visibility("default") logic) - same macro name, same include path, so
// nothing that includes it needs to change again.
#define AC3FORGE_EXPORT
