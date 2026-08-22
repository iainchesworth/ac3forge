// Encode a channel selection none of the eight named layouts covers.
//
// ac3::plan::LayoutId only ever names a short, hand-picked list (mono through
// 7.1.4). Plan::custom_locations is the escape hatch onto the general
// problem underneath: give ac3::eac3::chanmap::allocate any set of Table E2.5
// locations and it partitions them into a bed and however many dependents
// the remainder needs. Here that's 5.1 plus a top-surround channel and a
// front-wide pair — a layout no receiver profile names, built the same way a
// caller with an unusual speaker set would.

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/plan.hpp"

int main() {
    const auto locations = ac3::plan::parse_channels("L,C,R,Ls,Rs,LFE,Ts,Lw,Rw");
    if (!locations) {
        std::printf("parse_channels failed\n");
        return 1;
    }

    const ac3::plan::Plan plan{
        .codec = ac3::plan::Codec::kEac3,
        .custom_locations = *locations,
        .bitrate_kbps = 640,
    };
    if (const auto error = ac3::plan::validate(plan)) {
        std::printf("invalid plan: %.*s\n", static_cast<int>(ac3::plan::describe(*error).size()),
                    ac3::plan::describe(*error).data());
        return 1;
    }

    const auto channel_plan = ac3::plan::resolve(plan);
    const auto names = ac3::plan::coded_channel_names(channel_plan);
    std::printf("bed: %d full-bandwidth channel(s)%s, %zu dependent substream(s), %zu coded channels\n",
                ac3::fullbw_channel_count(channel_plan.bed_acmod), channel_plan.bed_lfe ? " + LFE" : "",
                channel_plan.dependents.size(), names.size());
    for (const auto& name : names) {
        std::printf("  %s\n", name.c_str());
    }

    const auto config = ac3::plan::eac3_config(plan);
    ac3::eac3::AccessUnitEncoder encoder{config};

    const auto channel_count = static_cast<std::size_t>(encoder.channel_count());
    std::vector<std::vector<float>> pcm(channel_count, std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> views;
    views.reserve(channel_count);
    for (auto& channel : pcm) {
        views.emplace_back(channel);
    }

    std::vector<std::byte> stream;
    for (int frame = 0; frame < 31; ++frame) {
        for (std::size_t ch = 0; ch < channel_count; ++ch) {
            const double tone = 400.0 + 300.0 * static_cast<double>(ch);
            for (int n = 0; n < ac3::kSamplesPerFrame; ++n) {
                const double t = (frame * ac3::kSamplesPerFrame + n) / 48000.0;
                pcm[ch][static_cast<std::size_t>(n)] =
                    static_cast<float>(0.4 * std::sin(2.0 * std::numbers::pi * tone * t));
            }
        }
        const auto unit = encoder.encode_access_unit(views);
        if (!unit) {
            std::printf("encode failed: %d\n", std::to_underlying(unit.error()));
            return 1;
        }
        stream.insert(stream.end(), unit->bytes.begin(), unit->bytes.end());
    }
    std::printf("%zu channels, %zu bytes\n", channel_count, stream.size());
    return 0;
}
