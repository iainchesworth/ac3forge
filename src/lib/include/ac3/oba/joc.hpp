#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ac3/export.hpp"
#include "ac3/oba/joc_tables.hpp"

// Joint Object Coding - ETSI TS 103 420 clause 6. The tool that gets more
// objects out of a decoder than there are channels in the bitstream.
//
// JOC codes no audio of its own. It carries a matrix: for each output object,
// how much of each downmix channel to take, per QMF parameter band and
// interpolated across the frame. The decoder (§6.6.6) computes
//     object[obj] = sum over channels of downmix[ch] * mix[obj][ch]
// in the complex QMF domain, so the "matrix" is really a set of per-band
// gains, and the whole tool is 5 channels in, up to 16 objects out.
//
// Because the reconstruction is a linear combination of the downmix, objects
// that were mixed into the SAME downmix channels with the same gains cannot be
// pulled apart again. JOC is a parametric approximation, not a lossless
// separation, and its quality depends entirely on how well-separated the
// objects were in the downmix.

namespace ac3::joc {

// Table 47 / Table 48. Only the 5.X configurations are reachable here: 7.X
// needs Lb/Rb in the downmix, which costs a dependent substream.
inline constexpr int kDmxConfig5X = 0;
inline constexpr int kNumChannels5X = 5;

// §7.1: the complex QMF the reconstruction runs in is 64 subbands wide.
inline constexpr int kQmfSubbands = 64;

// §6.3.2.4: joc_num_objects_bits is 6 bits but capped at 15, so 16 objects.
inline constexpr int kMaxObjects = 16;

// The reconstruction matrix for one frame, in the dequantized units §6.6.4
// produces - a range of roughly [-9,6; 9,5], not a normalized gain.
//
// The layout is [object][channel][band], row-major, which is the order
// joc_data writes it in.
struct FrameParameters {
    int objects = 0;
    int channels = kNumChannels5X;
    // Index into kNumBands (Table 50), not the band count itself.
    int num_bands_idx = 4;  // 9 bands
    // §6.3.3.7. Coarse is 96 quantization steps over the range, fine is 192.
    // Fine halves the step at roughly one extra bit per coefficient.
    bool fine_quant = false;
    // §6.3.3.3: a splice detector, not a timestamp. It counts frames from 1 to
    // 1023 and wraps to 1; 0 means "first frame, or first after a splice", so
    // the decoder knows joc_mix_mtx_prev is meaningless and must not
    // interpolate from it.
    int seq_count = 0;
    std::vector<double> matrix{};

    [[nodiscard]] int bands() const { return kNumBands[static_cast<std::size_t>(num_bands_idx)]; }
    [[nodiscard]] std::size_t coefficient_count() const {
        return static_cast<std::size_t>(objects) * static_cast<std::size_t>(channels) *
               static_cast<std::size_t>(bands());
    }
    [[nodiscard]] double& at(int object, int channel, int band) {
        return matrix[(static_cast<std::size_t>(object) * static_cast<std::size_t>(channels) +
                       static_cast<std::size_t>(channel)) *
                          static_cast<std::size_t>(bands()) +
                      static_cast<std::size_t>(band)];
    }
    [[nodiscard]] double at(int object, int channel, int band) const {
        return matrix[(static_cast<std::size_t>(object) * static_cast<std::size_t>(channels) +
                       static_cast<std::size_t>(channel)) *
                          static_cast<std::size_t>(bands()) +
                      static_cast<std::size_t>(band)];
    }
};

// §6.6.4's quantizer, and its inverse. The step is 820/(4096*(1+fine)) and the
// origin sits at nquant/2, so code nquant/2 is exactly zero gain.
[[nodiscard]] AC3FORGE_EXPORT int quantize(double coefficient, bool fine_quant);
[[nodiscard]] AC3FORGE_EXPORT double dequantize(int code, bool fine_quant);

// One joc() payload (§6.2.1), padded to whole bytes for emdf_payload_size.
[[nodiscard]] AC3FORGE_EXPORT std::vector<std::byte> build_payload(const FrameParameters& params);

// --- Decode ------------------------------------------------------------

// Decode-side inverse of build_payload(). Recognises exactly the shapes this
// encoder ever produces: a 5.X downmix, no extensional configuration data,
// unity clip gain, every object present every frame in whole-matrix (not
// sparse) mode with a single smooth-interpolation data point, and the SAME
// num_bands_idx/fine_quant for every object in the frame - the last because
// FrameParameters itself has no room to represent them varying per object,
// matching how AtmosEncoder only ever writes one shared value for both.
// Anything else is refused (std::nullopt) rather than guessed at, the same
// stance emdf::parse_container and oba::parse_payload take on their own
// unsupported configurations. `matrix` comes back already dequantized
// (§6.6.4's inverse) - the caller never sees the wire's Huffman codes.
[[nodiscard]] AC3FORGE_EXPORT std::optional<FrameParameters> parse_payload(
    std::span<const std::byte> payload);

// --- Audio reconstruction -----------------------------------------------

// §6.6.6's normative reconstruction runs in the complex QMF domain this
// codebase has no filterbank for; nothing here needs one specifically,
// because §6.6.6 is not normative about WHICH linear per-band transform
// computes it, only about the operation once you're in one. This applies it
// in the same 512-sample MDCT domain atmos.cpp's band_energy already uses to
// derive the matrix in the first place (see that function's own comment on
// why that substitution is legitimate for an encoder - here it is legitimate
// for the matching decoder for the identical reason: consistency with how
// the matrix was estimated matters far more than which linear transform a
// parametric, already-lossy tool happens to run in.
//
// Carried frame to frame for one program's worth of reconstruction, the same
// way Eac3Decoder's own overlap-add delay_ is: `bed_history` gives block 0 of
// each frame real pre-roll instead of zero-padding across the frame seam,
// and `object_history` is each object's own overlap-add tail. `previous_*`
// is what §6.6.5's ramp interpolates FROM; a shape mismatch against the
// frame just decoded (object count, band count) is treated exactly like
// FrameParameters::seq_count == 0 - no ramp, this frame's matrix applies to
// the whole frame outright - since there is nothing meaningful to ramp from.
struct ReconstructionState {
    std::array<std::array<double, 256>, kNumChannels5X> bed_history{};
    std::vector<double> previous_matrix{};
    int previous_objects = 0;
    int previous_num_bands_idx = -1;
    std::vector<std::array<double, 256>> object_history{};
};

// Reconstructs each JOC object's time-domain audio for one frame from the
// decoded downmix and this frame's parsed JOC parameters.
//
// `bed` must be exactly kNumChannels5X channels of kSamplesPerFrame samples
// each, in Table 53's JOC channel order (L, R, C, Ls, Rs) - NOT AC-3's
// Table 5.8 order (L, C, R, Ls, Rs); the caller permutes, the same
// permutation atmos.cpp's AtmosEncoder applies on the way in (see its
// kAc3FromJoc). Returns one waveform per object, `params.objects` of them,
// each kSamplesPerFrame samples, in the SAME order build_payload's own
// `objects`/matrix rows use - which, for a program this project's own
// AtmosEncoder produces (dynamic-object-only with a bypassed LFE, no bed),
// is exactly oba::DecodedProgram::objects' order too.
[[nodiscard]] AC3FORGE_EXPORT std::vector<std::vector<float>> reconstruct(
    std::span<const std::vector<float>> bed, const FrameParameters& params,
    ReconstructionState& state, bool fast_mdct = false);

}  // namespace ac3::joc
