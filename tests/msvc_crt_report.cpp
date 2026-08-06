// MSVC debug builds pop an interactive dialog on assert/abort by default,
// which hangs headless test runs until the CTest timeout (observed: a failed
// assert cost 6 minutes of wall clock instead of failing instantly). Route
// all CRT reports to stderr and make abort() terminate immediately.
#ifdef _MSC_VER

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

#endif
