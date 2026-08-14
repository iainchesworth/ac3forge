#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/export.hpp"
#include "ac3/oba/joc.hpp"
#include "ac3/oba/oamd.hpp"
#include "ac3/spatial/spatial.hpp"

// Dolby Atmos in Dolby Digital Plus: objects in, one ordinary-looking 5.1
// E-AC-3 stream out.
//
// The whole trick is that there is only one audio payload. Objects are panned
// into a 5.1 bed the way they always were, and that bed is what a legacy
// decoder plays - unchanged, at full level, complete. Alongside it ride two
// pieces of side data in an EMDF container: OAMD saying where each object is,
// and JOC saying how to pull the objects back out of the bed. A decoder that
// knows about neither is not merely tolerated, it is the design target.
//
// What JOC can and cannot do follows from that. The reconstruction is a linear
// combination of the five bed channels, so objects that landed on the same
// channels with the same gains cannot be separated again, however different
// they sounded going in. Elevation in particular costs nothing in the bed -
// the ring has no height speakers - so two objects at one azimuth and
// different heights are indistinguishable in the downmix, and the matrix can
// only split their shared energy in proportion to how loud each one is. That
// is not a defect of this encoder; it is what a parametric object coder is.

namespace ac3::oba {

struct AtmosConfig {
    SampleRate sample_rate = SampleRate::k48000;
    // The metadata competes with the mantissas for the same frame, so an
    // object stream needs headroom a plain 5.1 stream does not.
    std::uint32_t bitrate_kbps = 448;
    int dialnorm = 31;
    // Index into joc::kNumBands (Table 50). Nine bands resolve the spectrum
    // about as finely as a five-channel downmix can justify; more bands cost
    // codewords without giving the matrix anything new to say.
    int num_bands_idx = 4;
    // §6.3.3.7's finer quantizer: half the step, roughly one more bit per
    // coefficient. Worth it when objects are nearly degenerate and the matrix
    // entries are large.
    bool fine_quant = false;
    // Whether to emit the EMDF object container (OAMD + JOC) into block 0's
    // skip field. On by default: object-aware decoders get the objects, and a
    // decoder that ignores the container plays the 5.1 bed underneath it - the
    // design target described above.
    //
    // Turn it OFF to omit the container entirely. That is the only way to keep
    // the 5.1 bed playable on a decoder that VALIDATES the emdf_protection
    // field (TS 102 366 §H.2.2.4 leaves its contents "implementation dependent
    // and not defined", so a third-party encoder cannot satisfy such a check):
    // such a decoder treats the container's sync word as a commitment to object
    // decoding and refuses the whole stream if the field does not validate,
    // rather than falling back. With no container there is no sync word to find,
    // so it decodes the bed as ordinary 5.1. The choice is objects-or-nothing,
    // never both. The 5.1 MIX is the same either way (the same float bed is
    // encoded); the decoded samples are not bit-identical across the two,
    // because the frame's rate control gives the freed skip-field bytes back to
    // the mantissas, so the bed here is encoded at slightly higher fidelity.
    // See encode_frame().
    bool emit_object_metadata = true;
};

// One object's placement for one frame. Positions are room-anchored per
// §4.2.1; the gain is linear and applies to the bed as well as being sent as
// object_gain, because the object and its contribution to the downmix have to
// agree or the reconstruction is scaled wrong.
struct ObjectPlacement {
    Position position{};
    double gain = 1.0;
    // Objects never reach the LFE by panning (there is no direction that
    // points at it), so this is the only route - and it is deliberately not
    // part of the JOC matrix, because §6.3.2.2 bypasses the LFE entirely.
    double lfe_send = 0.0;
};

class AC3FORGE_EXPORT AtmosEncoder {
   public:
    AtmosEncoder(const AtmosConfig& config, int objects);

    // objects: one kSamplesPerFrame mono span per object, in the order the
    // encoder was constructed with. Returns one E-AC-3 access unit: a single
    // independent substream carrying the 5.1 bed, with the EMDF container in
    // its aux data.
    [[nodiscard]] std::expected<eac3::AccessUnit, FrameError> encode_frame(
        std::span<const std::span<const float>> objects,
        std::span<const ObjectPlacement> placement);

    // Dynamic objects only. The program has one more - the bed's LFE - which
    // is what the free object_count(Program) counts.
    [[nodiscard]] int dynamic_object_count() const { return objects_; }
    [[nodiscard]] const Program& program() const { return program_; }

    // The 5.1 bed the last frame encoded, in AC-3 coded order (L, C, R, Ls,
    // Rs, LFE). Exposed because it is what a legacy decoder hears, and that
    // is the thing most worth checking.
    [[nodiscard]] std::span<const std::vector<float>> bed() const { return bed_; }
    // The reconstruction matrix the last frame transmitted, before
    // quantization. Its channel axis is JOC's order (Table 53), not AC-3's.
    [[nodiscard]] const joc::FrameParameters& parameters() const { return params_; }

   private:
    AtmosConfig config_;
    int objects_ = 0;
    Program program_{};
    eac3::AccessUnitEncoder encoder_;
    joc::FrameParameters params_{};

    // Per object, its bed gains in JOC channel order plus its LFE send. Kept
    // between frames so the bed can ramp from where the last frame left off.
    std::vector<std::array<double, joc::kNumChannels5X>> gains_;
    std::vector<double> lfe_gains_;
    bool primed_ = false;

    std::vector<std::vector<float>> bed_;
    std::uint64_t frames_ = 0;
};

// Energy of one signal per JOC parameter band, over one frame's six blocks
// (§7's QMF re-expressed as the encoder's own 512-sample MDCT - see the
// definition's own comment for why that substitution is legitimate). Not
// part of AtmosEncoder's own public surface; exported so it can be measured
// in isolation (tests/performance/ac3kernelbench), the same reason
// eac3_tools.hpp exports ecpl_channel_spectrum and the AHT tools.
AC3FORGE_EXPORT void band_energy(std::span<const float> signal,
                                 std::span<const std::uint8_t, 64> mapping, std::span<double> out);

}  // namespace ac3::oba
