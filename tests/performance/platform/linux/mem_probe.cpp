#include "mem_probe.hpp"

#include <malloc.h>
#include <sys/resource.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>

namespace membench {

void* raw_alloc(std::size_t size) noexcept { return std::malloc(size != 0 ? size : 1); }

void raw_free(void* p) noexcept { std::free(p); }

std::size_t usable_size(void* p) noexcept { return malloc_usable_size(p); }

void* raw_alloc_aligned(std::size_t size, std::size_t align) noexcept {
    // posix_memalign requires the alignment to be a multiple of
    // sizeof(void*); the align_val_t overloads only fire for alignments
    // above __STDCPP_DEFAULT_NEW_ALIGNMENT__, but guard anyway.
    void* p = nullptr;
    if (posix_memalign(&p, align < sizeof(void*) ? sizeof(void*) : align,
                       size != 0 ? size : 1) != 0) {
        return nullptr;
    }
    return p;
}

void raw_free_aligned(void* p, std::size_t) noexcept { std::free(p); }

std::size_t usable_size_aligned(void* p, std::size_t) noexcept {
    // glibc's malloc_usable_size understands posix_memalign pointers.
    return malloc_usable_size(p);
}

ProcessMemory process_memory() noexcept {
    // Linux's ru_maxrss is in kibibytes (macOS reports bytes, which is one
    // of the reasons this file lives in platform/linux, not platform/posix).
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return {};
    }
    ProcessMemory result{
        .peak_rss_bytes = static_cast<unsigned long long>(usage.ru_maxrss) * 1024ULL,
        .current_rss_bytes = 0,
        .valid = true};

    // Current RSS is not in rusage; /proc/self/statm's second field is it,
    // in pages. Only ever called at report time, after every measurement
    // snapshot has been taken, so the ifstream's own allocations are free
    // to be counted - they perturb nothing.
    std::ifstream statm("/proc/self/statm");
    unsigned long long total_pages = 0;
    unsigned long long resident_pages = 0;
    if (statm >> total_pages >> resident_pages) {
        const long page = sysconf(_SC_PAGESIZE);
        result.current_rss_bytes = resident_pages * static_cast<unsigned long long>(page);
    }
    return result;
}

}  // namespace membench
