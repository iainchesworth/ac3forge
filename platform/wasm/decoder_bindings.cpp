// Embind wrapper around ac3::forge's decode path, for the roadmap-F3 browser
// demo (examples/wasm_decode_demo/index.html). One JS-visible class,
// `Decoder`: feed it a raw AC-3/E-AC-3 elementary stream (a Uint8Array,
// exactly what fetch().arrayBuffer() gives you - no container, no demux step,
// see ac3::io::scan's own header comment), and it decodes every access unit
// up front, exposing the concatenated PCM per channel plus a coarse per-block
// RMS energy trace per channel that the page uses to drive its speaker-ring
// visualization in sync with Web Audio playback.
//
// Scope note: this only surfaces the decoded BED (5.1/7.x) channels. Real
// per-object position/audio decode (OAMD/JOC) does not exist in ac3::forge
// yet - see the PR this file shipped in for the full explanation - so there
// is deliberately no object-position API here. index.html's visualization is
// therefore real per-channel bed energy, not object motion.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/io/elementary.hpp"

namespace {

// One RMS value per this many samples per channel - about 21ms at 48kHz,
// small enough for the visualization to feel responsive to the music without
// recomputing energy in JS on every animation frame.
constexpr std::size_t kEnergyBlockSamples = 1024;

std::vector<float> block_rms(const std::vector<float>& samples, std::size_t block_size) {
    std::vector<float> out;
    if (block_size == 0) {
        return out;
    }
    out.reserve(samples.size() / block_size + 1);
    for (std::size_t start = 0; start < samples.size(); start += block_size) {
        const std::size_t end = std::min(start + block_size, samples.size());
        double sum_sq = 0.0;
        for (std::size_t i = start; i < end; ++i) {
            const double s = static_cast<double>(samples[i]);
            sum_sq += s * s;
        }
        const double count = static_cast<double>(end - start);
        out.push_back(static_cast<float>(std::sqrt(sum_sq / count)));
    }
    return out;
}

}  // namespace

class WasmDecoder {
   public:
    // Decodes the whole stream up front. Returns true on success; on failure,
    // error() explains why and every other getter reports an empty result.
    bool decode(const emscripten::val& js_bytes) {
        error_.clear();
        channels_.clear();
        labels_.clear();
        stream_kind_.clear();
        sample_rate_ = 0;

        const std::vector<std::uint8_t> raw = emscripten::vecFromJSArray<std::uint8_t>(js_bytes);
        const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(raw.data()),
                                                raw.size());

        const auto scanned = ac3::io::scan(bytes);
        if (!scanned) {
            error_ = std::string(ac3::io::describe(scanned.error()));
            return false;
        }
        stream_kind_ = scanned->kind == ac3::io::StreamKind::kAc3 ? "AC-3" : "E-AC-3";
        sample_rate_ = static_cast<int>(ac3::sample_rate_hz(scanned->sample_rate));

        if (scanned->kind == ac3::io::StreamKind::kAc3) {
            ac3::FrameDecoder decoder;
            for (const auto unit : scanned->access_units) {
                const auto decoded = decoder.decode_frame(unit);
                if (!decoded) {
                    error_ = std::string(ac3::describe(decoded.error()));
                    return false;
                }
                if (labels_.empty()) {
                    labels_ = ac3_channel_labels(decoded->acmod, decoded->lfe);
                }
                append(decoded->channels);
            }
        } else {
            ac3::Eac3Decoder decoder;
            for (const auto unit : scanned->access_units) {
                const auto decoded = decoder.decode_access_unit(unit);
                if (!decoded) {
                    error_ = std::string(ac3::describe(decoded.error()));
                    return false;
                }
                if (decoded->has_value()) {
                    apply_layout(**decoded);
                }
            }
            for (const auto& flushed : decoder.flush()) {
                apply_layout_substream(flushed);
            }
        }

        energy_.clear();
        for (const auto& pcm : channels_) {
            energy_.push_back(block_rms(pcm, kEnergyBlockSamples));
        }
        return true;
    }

    [[nodiscard]] std::string error() const { return error_; }
    [[nodiscard]] std::string streamKind() const { return stream_kind_; }
    [[nodiscard]] int sampleRate() const { return sample_rate_; }
    [[nodiscard]] int channelCount() const { return static_cast<int>(channels_.size()); }
    [[nodiscard]] int energyBlockSize() const { return static_cast<int>(kEnergyBlockSamples); }

    [[nodiscard]] emscripten::val channelLabels() const {
        auto arr = emscripten::val::array();
        for (std::size_t i = 0; i < labels_.size(); ++i) {
            arr.set(static_cast<unsigned>(i), labels_[i]);
        }
        return arr;
    }

    // Zero-copy view into this instance's own PCM buffer - valid only until
    // the next decode() call or this object's destruction, exactly like any
    // other WASM heap view (see index.html, which reads it once right after
    // decode() and never holds onto it across a call).
    [[nodiscard]] emscripten::val channelPcm(int channel) const {
        if (channel < 0 || static_cast<std::size_t>(channel) >= channels_.size()) {
            return emscripten::val::null();
        }
        const auto& pcm = channels_[static_cast<std::size_t>(channel)];
        return emscripten::val(emscripten::typed_memory_view(pcm.size(), pcm.data()));
    }

    [[nodiscard]] emscripten::val channelEnergy(int channel) const {
        if (channel < 0 || static_cast<std::size_t>(channel) >= energy_.size()) {
            return emscripten::val::null();
        }
        const auto& blocks = energy_[static_cast<std::size_t>(channel)];
        return emscripten::val(emscripten::typed_memory_view(blocks.size(), blocks.data()));
    }

   private:
    static std::vector<std::string> ac3_channel_labels(ac3::Acmod acmod, bool lfe) {
        // Table 5.8 coded order; the decoder's own DecodedFrame/DecodedSubstream
        // header comments confirm LFE is always last when present.
        static const std::array<std::vector<std::string>, 8> kByAcmod{{
            {"Ch1", "Ch2"},              // kDualMono (1+1: two programmes, not a layout)
            {"C"},                       // k1_0
            {"L", "R"},                  // k2_0
            {"L", "C", "R"},             // k3_0
            {"L", "R", "S"},             // k2_1
            {"L", "C", "R", "S"},        // k3_1
            {"L", "R", "Ls", "Rs"},      // k2_2
            {"L", "C", "R", "Ls", "Rs"}  // k3_2
        }};
        auto labels = kByAcmod[static_cast<std::uint8_t>(acmod)];
        if (lfe) {
            labels.emplace_back("LFE");
        }
        return labels;
    }

    // DecodedAccessUnit's `layout` gives the real Table E2.5 channel identity
    // for each position - used in preference to acmod/lfe alone so a stream
    // with dependent substreams (7.1.4 etc.) still gets correct labels.
    void apply_layout(const ac3::DecodedAccessUnit& unit) {
        if (unit.layout.count > 0) {
            if (labels_.empty()) {
                labels_.clear();
                for (const auto location : unit.layout) {
                    labels_.emplace_back(ac3::eac3::chanmap::name(location));
                }
            }
        } else if (labels_.empty()) {
            // Dual mono: not a layout at all (see DecodedAccessUnit's own
            // comment) - fall back to acmod-based labels, which cover it.
            labels_ = ac3_channel_labels(unit.acmod, false);
        }
        append(unit.channels);
    }

    void apply_layout_substream(const ac3::DecodedSubstream& sub) {
        if (labels_.empty()) {
            const auto map = sub.location_map();
            const auto layout = ac3::eac3::chanmap::expand(map);
            for (const auto location : layout) {
                labels_.emplace_back(ac3::eac3::chanmap::name(location));
            }
        }
        append(sub.channels);
    }

    void append(const std::vector<std::vector<float>>& frame_channels) {
        if (channels_.size() < frame_channels.size()) {
            channels_.resize(frame_channels.size());
        }
        for (std::size_t ch = 0; ch < frame_channels.size(); ++ch) {
            auto& dst = channels_[ch];
            const auto& src = frame_channels[ch];
            dst.insert(dst.end(), src.begin(), src.end());
        }
    }

    std::string error_;
    std::string stream_kind_;
    int sample_rate_ = 0;
    std::vector<std::string> labels_;
    std::vector<std::vector<float>> channels_;
    std::vector<std::vector<float>> energy_;
};

EMSCRIPTEN_BINDINGS(ac3forge_wasm_decode) {
    emscripten::class_<WasmDecoder>("Decoder")
        .constructor<>()
        .function("decode", &WasmDecoder::decode)
        .function("error", &WasmDecoder::error)
        .function("streamKind", &WasmDecoder::streamKind)
        .function("sampleRate", &WasmDecoder::sampleRate)
        .function("channelCount", &WasmDecoder::channelCount)
        .function("channelLabels", &WasmDecoder::channelLabels)
        .function("channelPcm", &WasmDecoder::channelPcm)
        .function("channelEnergy", &WasmDecoder::channelEnergy)
        .function("energyBlockSize", &WasmDecoder::energyBlockSize);
}
