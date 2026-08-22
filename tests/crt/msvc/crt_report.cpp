// MSVC debug builds pop an interactive dialog on assert/abort by default,
// which hangs headless test runs until the CTest timeout (observed: a failed
// assert cost 6 minutes of wall clock instead of failing instantly). Route
// all CRT reports to stderr and make abort() terminate immediately.
//
// Every toolchain ships the same filename under platform/<toolchain>/; CMake
// compiles the directory that matches the target, so there is no #ifdef here.
// The axis is the CRT rather than the OS: <crtdbg.h> and the _Crt* reporting
// controls come with the MSVC runtime, so clang-cl gets this file too and a
// MinGW build gets the no-op in platform/stub/.

#include <crtdbg.h>
#include <cstdlib>

namespace {

struct CrtReportToStderr {
    CrtReportToStderr() {
        _set_error_mode(_OUT_TO_STDERR);
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
        _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
        _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    }
};

const CrtReportToStderr kInstall;

}  // namespace
