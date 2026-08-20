// Memory-trend data producer: the heap half of the performance suite,
// alongside bench_encoder.cpp's wall-clock numbers. Where ac3bench answers
// "how fast is a frame", this answers "how many heap allocations and bytes
// does a frame cost, and how much memory does a stream hold live" - churn
// and footprint, not time.
//
// Counting works by replacing the global allocation functions with
// counting wrappers, so every operator new/delete in the process - the
// harness AND the statically linked codec - is observed. That is also why
// this binary links ac3::forge_static explicitly: allocations inside a DLL
// bind to the DLL's own operator new at its link time, and an exe-side
// replacement would never see them.
//
// Windows-only for now (_msize/_aligned_msize and K32GetProcessMemoryInfo);
// a committed cross-platform version would take the platform-tree treatment.

#include <windows.h>
// windows.h first; psapi.h needs its types.
#include <psapi.h>

#include <malloc.h>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <new>
#include <numbers>
#include <span>
#include <string>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/oba/atmos.hpp"

namespace {

std::atomic<std::uint64_t> g_alloc_calls{0};
std::atomic<std::uint64_t> g_free_calls{0};
std::atomic<std::uint64_t> g_alloc_bytes{0};
std::atomic<std::int64_t> g_live_bytes{0};
std::atomic<std::int64_t> g_peak_live{0};

void note_alloc(std::size_t size) noexcept {
    g_alloc_calls.fetch_add(1, std::memory_order_relaxed);
    g_alloc_bytes.fetch_add(size, std::memory_order_relaxed);
    const std::int64_t live =
        g_live_bytes.fetch_add(static_cast<std::int64_t>(size), std::memory_order_relaxed) +
        static_cast<std::int64_t>(size);
    std::int64_t peak = g_peak_live.load(std::memory_order_relaxed);
    while (live > peak &&
           !g_peak_live.compare_exchange_weak(peak, live, std::memory_order_relaxed)) {
    }
}

void note_free(std::size_t size) noexcept {
    g_free_calls.fetch_add(1, std::memory_order_relaxed);
    g_live_bytes.fetch_sub(static_cast<std::int64_t>(size), std::memory_order_relaxed);
}

}  // namespace

void* operator new(std::size_t size) {
    void* p = std::malloc(size != 0 ? size : 1);
    if (p == nullptr) {
        throw std::bad_alloc{};
    }
    note_alloc(_msize(p));
    return p;
}

void* operator new[](std::size_t size) { return ::operator new(size); }

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    void* p = std::malloc(size != 0 ? size : 1);
    if (p != nullptr) {
        note_alloc(_msize(p));
    }
    return p;
}

void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept {
    return ::operator new(size, tag);
}

void operator delete(void* p) noexcept {
    if (p == nullptr) {
        return;
    }
    note_free(_msize(p));
    std::free(p);
}

void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { ::operator delete(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { ::operator delete(p); }

void* operator new(std::size_t size, std::align_val_t align) {
    void* p = _aligned_malloc(size != 0 ? size : 1, static_cast<std::size_t>(align));
    if (p == nullptr) {
        throw std::bad_alloc{};
    }
    note_alloc(_aligned_msize(p, static_cast<std::size_t>(align), 0));
    return p;
}

void* operator new[](std::size_t size, std::align_val_t align) {
    return ::operator new(size, align);
}

void operator delete(void* p, std::align_val_t align) noexcept {
    if (p == nullptr) {
        return;
    }
    note_free(_aligned_msize(p, static_cast<std::size_t>(align), 0));
    _aligned_free(p);
}

void operator delete[](void* p, std::align_val_t align) noexcept { ::operator delete(p, align); }
void operator delete(void* p, std::size_t, std::align_val_t align) noexcept {
    ::operator delete(p, align);
}
void operator delete[](void* p, std::size_t, std::align_val_t align) noexcept {
    ::operator delete(p, align);
}

namespace {

constexpr int kFrames = 200;
constexpr double kSampleRate = 48000.0;

struct Snap {
    std::uint64_t allocs = 0;
    std::uint64_t bytes = 0;
    std::int64_t live = 0;
    std::int64_t peak = 0;
};

Snap snap() {
    return {.allocs = g_alloc_calls.load(std::memory_order_relaxed),
            .bytes = g_alloc_bytes.load(std::memory_order_relaxed),
            .live = g_live_bytes.load(std::memory_order_relaxed),
            .peak = g_peak_live.load(std::memory_order_relaxed)};
}

void reset_peak() {
    g_peak_live.store(g_live_bytes.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

// Real-ish content on purpose: two detuned partials plus deterministic noise
// per channel. Silence (or a lone pure tone) under-exercises the mantissa
// and coupling paths and would understate per-frame costs.
std::vector<float> signal_frame(std::uint64_t& n, double base_freq, std::uint32_t& lcg) {
    std::vector<float> samples(static_cast<std::size_t>(ac3::kSamplesPerFrame));
    for (auto& s : samples) {
        const double t = static_cast<double>(n) / kSampleRate;
        lcg = lcg * 1664525U + 1013904223U;
        const double noise = (static_cast<double>(lcg >> 8) / 8388608.0 - 1.0) * 0.02;
        s = static_cast<float>(0.25 * std::sin(2.0 * std::numbers::pi * base_freq * t) +
                               0.12 * std::sin(2.0 * std::numbers::pi * base_freq * 2.71 * t) +
                               noise);
        ++n;
    }
    return samples;
}

struct Result {
    std::string name;
    int frames = 0;
    std::uint64_t setup_allocs = 0;
    std::uint64_t setup_bytes = 0;
    std::uint64_t first_allocs = 0;
    std::uint64_t first_bytes = 0;
    double allocs_per_frame = 0.0;  // steady state: frames 1..N-1
    double bytes_per_frame = 0.0;
    std::int64_t steady_live_growth = 0;  // live-byte drift across steady state
    std::int64_t peak_live_delta = 0;     // peak transient above pre-loop live
};

using Channels = std::vector<std::vector<float>>;

Channels make_channels(int count) {
    Channels channels;
    channels.reserve(static_cast<std::size_t>(count));
    std::uint32_t lcg = 0x2545F491U;
    for (int ch = 0; ch < count; ++ch) {
        std::uint64_t n = 0;
        channels.push_back(signal_frame(n, 110.0 * static_cast<double>(ch + 2), lcg));
    }
    return channels;
}

std::vector<std::span<const float>> make_views(const Channels& channels) {
    std::vector<std::span<const float>> views;
    views.reserve(channels.size());
    for (const auto& ch : channels) {
        views.emplace_back(ch);
    }
    return views;
}

template <typename Encoder, typename Encode>
Result run_encode(std::string name, Encoder& encoder, Encode encode,
                  std::vector<std::byte>* stream_out) {
    Result r{.name = std::move(name), .frames = kFrames};
    (void)encoder;

    const Snap before_first = snap();
    reset_peak();
    {
        const auto result = encode();
        if (!result) {
            std::fprintf(stderr, "%s: encode_frame failed\n", r.name.c_str());
            std::exit(1);
        }
        if (stream_out != nullptr) {
            stream_out->insert(stream_out->end(), result->begin(), result->end());
        }
    }
    const Snap after_first = snap();
    r.first_allocs = after_first.allocs - before_first.allocs;
    r.first_bytes = after_first.bytes - before_first.bytes;

    const Snap steady_start = snap();
    reset_peak();
    for (int frame = 1; frame < kFrames; ++frame) {
        const auto result = encode();
        if (!result) {
            std::fprintf(stderr, "%s: encode_frame failed\n", r.name.c_str());
            std::exit(1);
        }
        if (stream_out != nullptr) {
            stream_out->insert(stream_out->end(), result->begin(), result->end());
        }
    }
    const Snap steady_end = snap();
    r.allocs_per_frame =
        static_cast<double>(steady_end.allocs - steady_start.allocs) / (kFrames - 1);
    r.bytes_per_frame = static_cast<double>(steady_end.bytes - steady_start.bytes) / (kFrames - 1);
    r.steady_live_growth = steady_end.live - steady_start.live;
    r.peak_live_delta = steady_end.peak - steady_start.live;
    return r;
}

Result bench_ac3_51_encode(std::vector<std::byte>& stream_out) {
    const Channels channels = make_channels(6);
    const auto views = make_views(channels);
    stream_out.reserve(static_cast<std::size_t>(kFrames) * 2048);

    const Snap before = snap();
    ac3::FrameEncoder encoder{
        {.bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true, .fast_mdct = true}};
    const Snap after = snap();

    Result r = run_encode(
        "ac3_51_encode", encoder, [&] { return encoder.encode_frame(views); }, &stream_out);
    r.setup_allocs = after.allocs - before.allocs;
    r.setup_bytes = after.bytes - before.bytes;
    return r;
}

Result bench_eac3_51_encode(std::vector<std::byte>& stream_out) {
    const Channels channels = make_channels(6);
    const auto views = make_views(channels);
    stream_out.reserve(static_cast<std::size_t>(kFrames) * 2048);

    const Snap before = snap();
    ac3::eac3::FrameEncoder encoder{
        {.bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true}};
    const Snap after = snap();

    Result r = run_encode(
        "eac3_51_encode", encoder, [&] { return encoder.encode_frame(views); }, &stream_out);
    r.setup_allocs = after.allocs - before.allocs;
    r.setup_bytes = after.bytes - before.bytes;
    return r;
}

Result bench_atmos_4obj_encode(std::vector<std::byte>& stream_out) {
    constexpr int kObjects = 4;
    const Channels channels = make_channels(kObjects);
    const auto views = make_views(channels);
    std::vector<ac3::oba::ObjectPlacement> placement(static_cast<std::size_t>(kObjects));
    for (int obj = 0; obj < kObjects; ++obj) {
        placement[static_cast<std::size_t>(obj)] = {
            .position = {.x = 0.2 + 0.2 * obj, .y = 0.5, .z = 0.0}, .gain = 1.0};
    }
    stream_out.reserve(static_cast<std::size_t>(kFrames) * 4096);

    const Snap before = snap();
    ac3::oba::AtmosEncoder encoder{{.bitrate_kbps = 448, .fast_mdct = true}, kObjects};
    const Snap after = snap();

    Result r{.name = "atmos_4obj_encode", .frames = kFrames};
    r.setup_allocs = after.allocs - before.allocs;
    r.setup_bytes = after.bytes - before.bytes;

    const Snap before_first = snap();
    reset_peak();
    {
        const auto result = encoder.encode_frame(views, placement);
        if (!result) {
            std::fprintf(stderr, "atmos_4obj_encode: encode_frame failed\n");
            std::exit(1);
        }
        stream_out.insert(stream_out.end(), result->bytes.begin(), result->bytes.end());
    }
    const Snap after_first = snap();
    r.first_allocs = after_first.allocs - before_first.allocs;
    r.first_bytes = after_first.bytes - before_first.bytes;

    const Snap steady_start = snap();
    reset_peak();
    for (int frame = 1; frame < kFrames; ++frame) {
        const auto result = encoder.encode_frame(views, placement);
        if (!result) {
            std::fprintf(stderr, "atmos_4obj_encode: encode_frame failed\n");
            std::exit(1);
        }
        stream_out.insert(stream_out.end(), result->bytes.begin(), result->bytes.end());
    }
    const Snap steady_end = snap();
    r.allocs_per_frame =
        static_cast<double>(steady_end.allocs - steady_start.allocs) / (kFrames - 1);
    r.bytes_per_frame = static_cast<double>(steady_end.bytes - steady_start.bytes) / (kFrames - 1);
    r.steady_live_growth = steady_end.live - steady_start.live;
    r.peak_live_delta = steady_end.peak - steady_start.live;
    return r;
}

Result bench_ac3_51_decode(std::span<const std::byte> stream) {
    const auto frames = ac3::split_frames(stream);
    if (!frames || frames->empty()) {
        std::fprintf(stderr, "ac3_51_decode: split_frames failed\n");
        std::exit(1);
    }

    Result r{.name = "ac3_51_decode", .frames = static_cast<int>(frames->size())};

    const Snap before = snap();
    ac3::FrameDecoder decoder{};
    const Snap after = snap();
    r.setup_allocs = after.allocs - before.allocs;
    r.setup_bytes = after.bytes - before.bytes;

    const Snap before_first = snap();
    reset_peak();
    {
        const auto result = decoder.decode_frame((*frames)[0]);
        if (!result) {
            std::fprintf(stderr, "ac3_51_decode: decode_frame failed\n");
            std::exit(1);
        }
    }
    const Snap after_first = snap();
    r.first_allocs = after_first.allocs - before_first.allocs;
    r.first_bytes = after_first.bytes - before_first.bytes;

    const Snap steady_start = snap();
    reset_peak();
    for (std::size_t i = 1; i < frames->size(); ++i) {
        const auto result = decoder.decode_frame((*frames)[i]);
        if (!result) {
            std::fprintf(stderr, "ac3_51_decode: decode_frame failed\n");
            std::exit(1);
        }
    }
    const Snap steady_end = snap();
    const auto steady_frames = static_cast<double>(frames->size() - 1);
    r.allocs_per_frame = static_cast<double>(steady_end.allocs - steady_start.allocs) / steady_frames;
    r.bytes_per_frame = static_cast<double>(steady_end.bytes - steady_start.bytes) / steady_frames;
    r.steady_live_growth = steady_end.live - steady_start.live;
    r.peak_live_delta = steady_end.peak - steady_start.live;
    return r;
}

Result bench_eac3_decode(std::string name, std::span<const std::byte> stream) {
    const auto units = ac3::split_access_units(stream);
    if (!units || units->empty()) {
        std::fprintf(stderr, "%s: split_access_units failed\n", name.c_str());
        std::exit(1);
    }

    Result r{.name = std::move(name), .frames = static_cast<int>(units->size())};

    const Snap before = snap();
    ac3::Eac3Decoder decoder{};
    const Snap after = snap();
    r.setup_allocs = after.allocs - before.allocs;
    r.setup_bytes = after.bytes - before.bytes;

    const Snap before_first = snap();
    reset_peak();
    {
        const auto result = decoder.decode_access_unit((*units)[0]);
        if (!result) {
            std::fprintf(stderr, "%s: decode_access_unit failed\n", r.name.c_str());
            std::exit(1);
        }
    }
    const Snap after_first = snap();
    r.first_allocs = after_first.allocs - before_first.allocs;
    r.first_bytes = after_first.bytes - before_first.bytes;

    const Snap steady_start = snap();
    reset_peak();
    for (std::size_t i = 1; i < units->size(); ++i) {
        const auto result = decoder.decode_access_unit((*units)[i]);
        if (!result) {
            std::fprintf(stderr, "%s: decode_access_unit failed\n", r.name.c_str());
            std::exit(1);
        }
    }
    const auto rest = decoder.flush();
    (void)rest;
    const Snap steady_end = snap();
    const auto steady_units = static_cast<double>(units->size() - 1);
    r.allocs_per_frame = static_cast<double>(steady_end.allocs - steady_start.allocs) / steady_units;
    r.bytes_per_frame = static_cast<double>(steady_end.bytes - steady_start.bytes) / steady_units;
    r.steady_live_growth = steady_end.live - steady_start.live;
    r.peak_live_delta = steady_end.peak - steady_start.live;
    return r;
}

void write_json(const std::vector<Result>& results, const std::string& path,
                const PROCESS_MEMORY_COUNTERS& pmc) {
    std::ofstream out(path);
    out << "{\n  \"peak_working_set_bytes\": " << pmc.PeakWorkingSetSize
        << ",\n  \"results\": [\n";
    for (std::size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        out << "    {\"name\": \"" << r.name << "\", \"frames\": " << r.frames
            << ", \"setup_allocs\": " << r.setup_allocs << ", \"setup_bytes\": " << r.setup_bytes
            << ", \"first_allocs\": " << r.first_allocs << ", \"first_bytes\": " << r.first_bytes
            << ", \"allocs_per_frame\": " << r.allocs_per_frame
            << ", \"bytes_per_frame\": " << r.bytes_per_frame
            << ", \"steady_live_growth\": " << r.steady_live_growth
            << ", \"peak_live_delta\": " << r.peak_live_delta << "}"
            << (i + 1 < results.size() ? "," : "") << "\n";
    }
    out << "  ]\n}\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string json_out;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--json-out" && i + 1 < argc) {
            json_out = argv[++i];
        }
    }

    std::vector<Result> results;
    std::vector<std::byte> ac3_stream;
    std::vector<std::byte> eac3_stream;
    std::vector<std::byte> atmos_stream;

    results.push_back(bench_ac3_51_encode(ac3_stream));
    results.push_back(bench_ac3_51_decode(ac3_stream));
    results.push_back(bench_eac3_51_encode(eac3_stream));
    results.push_back(bench_eac3_decode("eac3_51_decode", eac3_stream));
    results.push_back(bench_atmos_4obj_encode(atmos_stream));
    results.push_back(bench_eac3_decode("atmos_4obj_decode", atmos_stream));

    std::printf(
        "%-18s %7s | %9s %12s | %9s %12s | %11s %13s | %12s %11s\n", "workload", "frames",
        "setup#", "setup B", "first#", "first B", "steady #/fr", "steady B/fr", "live growth",
        "peak delta");
    for (const auto& r : results) {
        std::printf(
            "%-18s %7d | %9llu %12llu | %9llu %12llu | %11.1f %13.0f | %12lld %11lld\n",
            r.name.c_str(), r.frames, static_cast<unsigned long long>(r.setup_allocs),
            static_cast<unsigned long long>(r.setup_bytes),
            static_cast<unsigned long long>(r.first_allocs),
            static_cast<unsigned long long>(r.first_bytes), r.allocs_per_frame, r.bytes_per_frame,
            static_cast<long long>(r.steady_live_growth),
            static_cast<long long>(r.peak_live_delta));
    }

    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)) != 0) {
        std::printf("\npeak working set: %.1f MiB   current: %.1f MiB\n",
                    static_cast<double>(pmc.PeakWorkingSetSize) / (1024.0 * 1024.0),
                    static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0));
    }

    if (!json_out.empty()) {
        write_json(results, json_out, pmc);
        std::printf("wrote %s\n", json_out.c_str());
    }

    return 0;
}
