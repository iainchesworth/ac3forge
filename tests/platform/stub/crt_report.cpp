// The no-CRT-hook build. Every toolchain ships the same filename under
// platform/<toolchain>/; CMake compiles this directory for runtimes with no
// interactive assert dialog to suppress, so there is no #ifdef here.
//
// Nothing to install: only the MSVC runtime turns a failed assert into a
// modal dialog that hangs a headless CTest run. Elsewhere assert() and
// abort() already write to stderr and terminate, which is the behaviour
// platform/msvc/crt_report.cpp exists to restore.
//
// Deliberately an empty translation unit.
