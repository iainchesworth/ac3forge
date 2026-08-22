#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "ac3/export.hpp"
#include "ac3/mlp/stream.hpp"
#include "ac3/oba/oamd.hpp"

// Atmos over TrueHD, assembled from the pieces this tree already ships: the
// bed and every dynamic object ride as their own DISCRETE lossless channels
// (the codec has room for them - no JOC, no parametric reconstruction, no
// shared-direction ambiguity), 16ch_channel_meaning() in every major sync
// says which channel is which (§4.4), and the per-frame object positions
// ride an EMDF container (TS 102 366 Annex H, ac3::emdf - the identical
// carrier-agnostic code the E-AC-3 Atmos path uses) holding an OAMD payload
// (TS 103 420 §5.5.2, ac3::oba) inside EXTRA_DATA() (§4.8).
//
// The counterpart of ac3::oba::AtmosEncoder with the carrier swapped: that
// one pans objects into a 5.1 bed and sends JOC coefficients to pull them
// back out; this one just encodes them. What is NOT settled by public
// sources is whether shipping TrueHD wraps its object metadata in
// EXTRA_DATA() as bare EMDF or in a further proprietary wrapper - the
// initiative's one remaining metadata gap (docs/concepts/truehd-mlp.md), so
// the EMDF-in-EXTRA_DATA choice here is provisional the same way the block
// header's field order is.

namespace ac3::mlp {

// Table 18's 16ch_channel_assignment bits - which loudspeaker feeds lead
// the presentation, in this exact channel order on the wire. The label set
// is identical to OAMD's bed assignment (TS 103 420 Table 12), just with
// the bit significance reversed; the encoder derives the OAMD mask from
// this one, so a caller states the bed exactly once.
namespace bed16 {

inline constexpr std::uint16_t kLR = 1 << 0;  // 2 channels
inline constexpr std::uint16_t kC = 1 << 1;
inline constexpr std::uint16_t kLfe = 1 << 2;
inline constexpr std::uint16_t kLsRs = 1 << 3;    // 2
inline constexpr std::uint16_t kLbRb = 1 << 4;    // 2
inline constexpr std::uint16_t kTflTfr = 1 << 5;  // 2
inline constexpr std::uint16_t kTslTsr = 1 << 6;  // 2
inline constexpr std::uint16_t kTblTbr = 1 << 7;  // 2
inline constexpr std::uint16_t kLwRw = 1 << 8;    // 2
inline constexpr std::uint16_t kLfe2 = 1 << 9;

inline constexpr std::uint16_t k51 = kLR | kC | kLfe | kLsRs;
inline constexpr std::uint16_t k714 = k51 | kLbRb | kTflTfr | kTblTbr;

}  // namespace bed16

struct AtmosConfig {
    SampleRate sample_rate = SampleRate::k48000;
    int wordlength = 24;          // 2..24, both bed and object channels
    int major_sync_interval = 8;  // 8..128 per §2.5

    // The loudspeaker-feed channels leading the presentation (Table 18
    // mask), 0 for an object-only programme. A bed of exactly bed16::kLfe
    // takes §4.4.4's efficient dyn_object_only + lfe_present form on the
    // wire; anything else is spelled out via 16ch_content_description.
    std::uint16_t bed = 0;
    // Discrete dynamic-object channels following the bed. The presentation
    // total (bed channels + objects) must be 1..16 (§4.4.3).
    int dynamic_objects = 1;

    // Access units between OAMD emissions: every metadata_interval-th unit
    // (starting with the first, which every decoder is guaranteed to see -
    // it carries the major sync) gets the EMDF container; the rest carry no
    // EXTRA_DATA. 1 sends positions with every 40-sample frame; the default
    // matches the major-sync cadence (a 150 Hz update rate at 48 kHz, far
    // finer than any authored motion, at an eighth of the metadata cost).
    // How often SHIPPING TrueHD sends OAMD is part of the unresolved
    // wrapper question above - this knob is the honest interim.
    int metadata_interval = 8;

    std::uint8_t dialogue_norm = 31;  // §4.4.1; 0 and 31 both mean -31 LKFS
    std::uint8_t mix_level = 0;       // §4.4.2; peak mixing level 70+value dB

    // §4.2.6 passthrough for two-pass callers, exactly as StreamConfig's.
    std::uint32_t peak_data_rate_16ths = 0;
};

class AC3FORGE_EXPORT AtmosEncoder {
   public:
    explicit AtmosEncoder(const AtmosConfig& config);

    // Bed channels + objects, one presentation: how many channel spans every
    // encode call must supply, in Table 18 order then object order.
    [[nodiscard]] int channel_count() const { return channel_count_; }

    // One access unit: samples_per_access_unit(rate) samples per channel,
    // bed feeds first (Table 18 order) then dynamic objects, plus each
    // object's position/gain for this frame (exactly
    // config.dynamic_objects of them - evaluated wherever the caller likes,
    // but only units on the metadata_interval cadence put them on the wire).
    [[nodiscard]] std::vector<std::byte> encode_access_unit(
        std::span<const std::span<const std::int32_t>> channels,
        std::span<const oba::DynamicObject> objects);

    // The final access unit: adds §4.6's terminators, same contract as
    // StreamEncoder's EndOfStream overload.
    [[nodiscard]] std::vector<std::byte> encode_access_unit(
        std::span<const std::span<const std::int32_t>> channels,
        std::span<const oba::DynamicObject> objects, EndOfStream end);

    [[nodiscard]] std::uint32_t measured_peak_data_rate_16ths() const {
        return encoder_.measured_peak_data_rate_16ths();
    }
    [[nodiscard]] std::uint64_t rate_violations() const { return encoder_.rate_violations(); }

   private:
    [[nodiscard]] std::vector<std::byte> metadata(std::span<const oba::DynamicObject> objects);

    AtmosConfig config_;
    int channel_count_;
    oba::Program program_;
    std::uint64_t unit_index_ = 0;
    StreamEncoder encoder_;
};

}  // namespace ac3::mlp
