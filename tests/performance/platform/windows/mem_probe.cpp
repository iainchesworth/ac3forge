#include "mem_probe.hpp"

#include <windows.h>
// windows.h first; psapi.h needs its types.
#include <psapi.h>

#include <malloc.h>

#include <cstdlib>

namespace membench {

void* raw_alloc(std::size_t size) noexcept { return std::malloc(size != 0 ? size : 1); }

void raw_free(void* p) noexcept { std::free(p); }

std::size_t usable_size(void* p) noexcept { return _msize(p); }

void* raw_alloc_aligned(std::size_t size, std::size_t align) noexcept {
    return _aligned_malloc(size != 0 ? size : 1, align);
}

void raw_free_aligned(void* p, std::size_t) noexcept { _aligned_free(p); }

std::size_t usable_size_aligned(void* p, std::size_t align) noexcept {
    return _aligned_msize(p, align, 0);
}

ProcessMemory process_memory() noexcept {
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)) == 0) {
        return {};
    }
    return {.peak_rss_bytes = pmc.PeakWorkingSetSize,
            .current_rss_bytes = pmc.WorkingSetSize,
            .valid = true};
}

}  // namespace membench
