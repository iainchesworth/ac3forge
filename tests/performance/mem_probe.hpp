#pragma once

#include <cstddef>

// The per-platform seam under bench_memory.cpp's replaced global allocation
// functions: raw allocation, exact usable-size introspection (what the
// allocator really handed out, so live-byte accounting balances even when
// operator delete never learns the requested size), and the process's
// resident-set numbers. One implementation per platform directory
// (platform/windows, platform/linux), selected by CMake - this repo's
// platform-tree rule, no #ifdefs.

namespace membench {

// malloc-family allocation. A size of zero is bumped to one so the returned
// pointer is always non-null on success and always introspectable.
[[nodiscard]] void* raw_alloc(std::size_t size) noexcept;
void raw_free(void* p) noexcept;
// Exact allocator-reported size for a raw_alloc pointer. p must be non-null.
[[nodiscard]] std::size_t usable_size(void* p) noexcept;

// Over-aligned allocation for the align_val_t operator new overloads.
// align is the alignment operator new was invoked with; the same value must
// be passed back to raw_free_aligned/usable_size_aligned (Windows's
// _aligned_msize requires it; POSIX ignores it).
[[nodiscard]] void* raw_alloc_aligned(std::size_t size, std::size_t align) noexcept;
void raw_free_aligned(void* p, std::size_t align) noexcept;
[[nodiscard]] std::size_t usable_size_aligned(void* p, std::size_t align) noexcept;

struct ProcessMemory {
    unsigned long long peak_rss_bytes = 0;
    unsigned long long current_rss_bytes = 0;
    bool valid = false;
};

// Peak and current resident set of this process, in bytes.
[[nodiscard]] ProcessMemory process_memory() noexcept;

}  // namespace membench
