#include "atmos.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <fstream>
#include <numbers>
#include <optional>
#include <print>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../support.hpp"
#include "ac3/analysis/levels.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/io/wav.hpp"
#include "ac3/oba/atmos.hpp"
#include "ac3/oba/motion.hpp"
#include "ac3/oba/oamd.hpp"
#include "ac3/signing/emdf_atmos_signer.hpp"
#include "ac3/signing/signing_key.hpp"
#include "../adm/atmos_adm.hpp"

namespace ac3cli::commands {

namespace {

// Applies EMDF object signing to freshly-encoded Atmos units when the operator
// asked for it (sign-objects) and supplied a key. Returns the number of frames
// signed, or nullopt if signing was requested but the key could not be loaded
// (the message is already printed). Not requested -> 0, units untouched. The
// key comes from the operator at runtime (signing-key=<path> or the
// AC3FORGE_SIGNING_KEY[_FILE] env vars) and is never stored - see
// docs/concepts/object-signing.md.
std::optional<int> apply_object_signing(std::vector<std::vector<std::byte>>& units,
                                        const Options& meta) {
    if (!meta.sign_objects) {
        return 0;
    }
    const auto key = ac3::signing::load_signing_key(meta.signing_key.value_or(""));
    if (!key) {
        if (key.error().kind == ac3::signing::KeyErrorKind::kAbsent) {
            std::println(stderr,
                         "error: sign-objects needs a key — pass signing-key=<path>, or set "
                         "AC3FORGE_SIGNING_KEY_FILE / AC3FORGE_SIGNING_KEY");
        } else {
            std::println(stderr, "error: {}", key.error().message);
        }
        return std::nullopt;
    }
    int signed_count = 0;
    for (auto& unit : units) {
        signed_count += ac3::signing::sign_atmos_stream(unit, *key);
    }
    return signed_count;
}

// Parses a hand-authored keyframe file: whitespace-separated columns
// "object_index time_s x y z gain lfe_send" per line, blank lines and '#'
// comments (to end of line) skipped. Returns each object's keyframes, indexed
// by object_index - an object index with no lines simply gets an empty entry.
std::optional<std::vector<std::vector<ac3::oba::Keyframe>>> parse_path_file(
    std::string_view path) {
    std::ifstream in{std::string{path}};
    if (!in) {
        std::println(stderr, "error: cannot open {}", path);
        return std::nullopt;
    }
    std::vector<std::vector<ac3::oba::Keyframe>> by_object;
    std::string line;
    for (std::size_t lineno = 1; std::getline(in, line); ++lineno) {
        if (const auto hash = line.find('#'); hash != std::string::npos) {
            line.resize(hash);
        }
        std::istringstream tokens{line};
        std::size_t object = 0;
        if (!(tokens >> object)) {
            continue;  // blank, or comment-only, line
        }
        ac3::oba::Keyframe kf;
        if (!(tokens >> kf.time_s >> kf.position.x >> kf.position.y >> kf.position.z >>
              kf.gain >> kf.lfe_send)) {
            std::println(stderr, "error: {}:{}: expected 'object time_s x y z gain lfe_send'",
                         path, lineno);
            return std::nullopt;
        }
        if (object >= by_object.size()) {
            by_object.resize(object + 1);
        }
        by_object[object].push_back(kf);
    }
    return by_object;
}

}  // namespace

int run_atmos(std::string_view out_path, std::uint32_t seconds, std::uint32_t bitrate,
              std::uint32_t objects, std::uint32_t orbit_seconds, std::string_view mode,
              const Options& meta) {
    if (objects < 1 || objects > 15) {
        std::println(stderr, "error: 1 to 15 objects (the bed's LFE is the 16th, "
                             "and TS 103 420 §8.3.2.2 caps the total at 16)");
        return 1;
    }
    // "objects" emits the JOC + OAMD container; "bed51" omits it so the stream
    // degrades to a plain 5.1 bed on a decoder that refuses an unvalidated
    // object container instead of falling back (see AtmosConfig).
    if (mode != "objects" && mode != "bed51") {
        std::println(stderr, "error: mode is 'objects' (default) or 'bed51'");
        return 1;
    }
    const bool emit_objects = mode != "bed51";
    const auto count = static_cast<std::size_t>(objects);
    ac3::oba::AtmosEncoder encoder{{.bitrate_kbps = bitrate,
                                    .dialnorm = meta.p.dialnorm,
                                    .num_bands_idx = 4,
                                    .emit_object_metadata = emit_objects,
                                    .fast_mdct = meta.fast_mdct},
                                   static_cast<int>(objects)};

    // Distinct tones so the objects are separable in the first place, and a
    // reader with an object renderer can tell which one ended up where.
    std::vector<double> tone_hz(count);
    std::vector<ac3::oba::ObjectPath> paths;
    paths.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        tone_hz[i] = 220.0 * std::pow(2.0, static_cast<double>(i) * 0.45);
        // Rates that are not simple ratios of each other, so the objects do
        // not lock into formation and stay separable.
        const double rate = 1.0 / (static_cast<double>(orbit_seconds) *
                                   (1.0 + 0.31 * static_cast<double>(i)));
        // Spread around the ring to begin with, or a short clip would show
        // them all bunched in the same quadrant - and objects that share a
        // direction are exactly the ones JOC cannot separate.
        const double phase = 2.0 * std::numbers::pi * static_cast<double>(i) /
                             static_cast<double>(count);
        const double height = count == 1 ? 0.5
                                         : -1.0 + 2.0 * static_cast<double>(i) /
                                                      static_cast<double>(count - 1);
        paths.push_back(ac3::oba::make_orbit_path(
            rate, phase, height, 0.7 / std::sqrt(static_cast<double>(count)),
            // Only the lowest object feeds the LFE, and only a little: it is
            // the one channel JOC never touches.
            i == 0 ? 0.2 : 0.0));
    }

    const std::uint64_t frames = (static_cast<std::uint64_t>(seconds) * 48000 + 1535) / 1536;
    std::vector<std::vector<float>> essences(count,
                                             std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> views(count);
    // Streamed out as encoded unless sign-objects defers them (the signing
    // pass below rewrites every frame after the loop). keep_partial is
    // hard-off: this command has never honoured keep-partial - its output
    // is synthetic and regenerable - so a mid-run failure must keep
    // leaving no file behind, which is exactly what abort() then does.
    EncodedStreamSink out_sink;
    if (!out_sink.open(out_path, /*keep_partial=*/false, /*defer=*/meta.sign_objects)) {
        return 1;
    }

    std::uint64_t n0 = 0;
    for (std::uint64_t f = 0; f < frames; ++f) {
        // The placement is the object's position at the END of the frame,
        // because that is where both metadata layers interpolate to: OAMD's
        // ramp and the JOC matrix both finish there.
        const double t = static_cast<double>(n0 + ac3::kSamplesPerFrame) / 48000.0;
        const auto placement = ac3::oba::evaluate_placements(paths, t);
        for (std::size_t i = 0; i < count; ++i) {
            for (int n = 0; n < ac3::kSamplesPerFrame; ++n) {
                essences[i][static_cast<std::size_t>(n)] = static_cast<float>(
                    std::sin(2.0 * std::numbers::pi * tone_hz[i] *
                             static_cast<double>(n0 + static_cast<std::uint64_t>(n)) / 48000.0));
            }
            views[i] = essences[i];
        }
        n0 += ac3::kSamplesPerFrame;

        auto unit = encoder.encode_frame(views, placement);
        if (!unit) {
            std::println(stderr,
                         "error: cannot encode {} objects at {} kbps — the metadata and "
                         "the mantissas share one frame, so try a higher bit rate",
                         objects, bitrate);
            out_sink.abort();
            return 1;
        }
        if (!out_sink.push(std::move(unit->bytes))) {
            out_sink.abort();
            return 1;
        }
    }
    // Optional object signing: writes the keyed EMDF-protection tag so a
    // decoder that validates it accepts the JOC objects instead of falling
    // back to the 5.1 bed. Off unless the operator passes sign-objects with a
    // key; the algorithm is in-tree (clean-room), only the key is supplied.
    // A key failure discards everything, as it always has - nothing is on
    // disk in defer mode, so a plain return leaves exactly no file.
    const auto signed_count = apply_object_signing(out_sink.deferred(), meta);
    if (!signed_count) {
        return 1;
    }
    if (*signed_count > 0) {
        std::println("  signed {} frames' EMDF object container with the supplied key",
                     *signed_count);
    }
    if (!out_sink.close()) {
        return 1;
    }
    std::println("wrote {} E-AC-3 access units to {}", frames, out_path);
    if (emit_objects) {
        std::println("  {} dynamic objects + the bed's LFE = {} objects, JOC over a 5.1 downmix",
                     objects, ac3::oba::object_count(encoder.program()));
    } else {
        std::println("  bed51: 5.1 bed only, no object container — plays as 5.1 on a decoder "
                     "that rejects an unvalidated one ({} objects were panned into the bed)",
                     objects);
    }
    return 0;
}

int run_atmos_path(std::string_view out_path, std::string_view paths_path, std::uint32_t seconds,
                   std::uint32_t bitrate, std::uint32_t objects_arg, const Options& meta) {
    const auto parsed = parse_path_file(paths_path);
    if (!parsed) {
        return 1;
    }
    const auto objects =
        objects_arg != 0 ? static_cast<std::size_t>(objects_arg) : parsed->size();
    if (objects < 1 || objects > 15) {
        std::println(stderr, "error: 1 to 15 objects (the bed's LFE is the 16th, "
                             "and TS 103 420 §8.3.2.2 caps the total at 16)");
        return 1;
    }
    if (parsed->size() > objects) {
        std::println(stderr,
                     "error: {} has keyframes up to object index {}, more than the {} objects "
                     "requested",
                     paths_path, parsed->size() - 1, objects);
        return 1;
    }

    std::vector<ac3::oba::ObjectPath> paths;
    paths.reserve(objects);
    for (std::size_t i = 0; i < objects; ++i) {
        if (i < parsed->size() && !(*parsed)[i].empty()) {
            auto created = ac3::oba::KeyframePath::create((*parsed)[i]);
            if (!created) {
                std::println(stderr, "error: object {} has two keyframes at the same time_s", i);
                return 1;
            }
            paths.emplace_back(std::move(*created));
            continue;
        }
        auto fallback = ac3::oba::KeyframePath::create(
            {{.time_s = 0.0,
              .position = {.x = 0.5, .y = 0.5, .z = 0.0},
              .gain = 0.7 / std::sqrt(static_cast<double>(objects)),
              .lfe_send = 0.0}});
        paths.emplace_back(std::move(*fallback));
    }

    ac3::oba::AtmosEncoder encoder{
        {.bitrate_kbps = bitrate, .dialnorm = meta.p.dialnorm, .num_bands_idx = 4,
         .fast_mdct = meta.fast_mdct},
        static_cast<int>(objects)};

    // Distinct tones purely for audibility, same as 'atmos'.
    std::vector<double> tone_hz(objects);
    for (std::size_t i = 0; i < objects; ++i) {
        tone_hz[i] = 220.0 * std::pow(2.0, static_cast<double>(i) * 0.45);
    }

    const std::uint64_t frames = (static_cast<std::uint64_t>(seconds) * 48000 + 1535) / 1536;
    std::vector<std::vector<float>> essences(objects,
                                             std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> views(objects);
    // Same output arrangement as 'atmos' above, keep_partial hard-off for
    // the same synthetic-and-regenerable reason.
    EncodedStreamSink out_sink;
    if (!out_sink.open(out_path, /*keep_partial=*/false, /*defer=*/meta.sign_objects)) {
        return 1;
    }

    std::uint64_t n0 = 0;
    for (std::uint64_t f = 0; f < frames; ++f) {
        const double t = static_cast<double>(n0 + ac3::kSamplesPerFrame) / 48000.0;
        const auto placement = ac3::oba::evaluate_placements(paths, t);
        for (std::size_t i = 0; i < objects; ++i) {
            for (int n = 0; n < ac3::kSamplesPerFrame; ++n) {
                essences[i][static_cast<std::size_t>(n)] = static_cast<float>(
                    std::sin(2.0 * std::numbers::pi * tone_hz[i] *
                             static_cast<double>(n0 + static_cast<std::uint64_t>(n)) / 48000.0));
            }
            views[i] = essences[i];
        }
        n0 += ac3::kSamplesPerFrame;

        auto unit = encoder.encode_frame(views, placement);
        if (!unit) {
            std::println(stderr,
                         "error: cannot encode {} objects at {} kbps — the metadata and "
                         "the mantissas share one frame, so try a higher bit rate",
                         objects, bitrate);
            out_sink.abort();
            return 1;
        }
        if (!out_sink.push(std::move(unit->bytes))) {
            out_sink.abort();
            return 1;
        }
    }
    // Optional object signing, same as 'atmos' - see the comments at its
    // call site there, the key-failure plain return included.
    const auto signed_count = apply_object_signing(out_sink.deferred(), meta);
    if (!signed_count) {
        return 1;
    }
    if (*signed_count > 0) {
        std::println("  signed {} frames' EMDF object container with the supplied key",
                     *signed_count);
    }
    if (!out_sink.close()) {
        return 1;
    }
    std::println("wrote {} E-AC-3 access units to {} ({} objects from {})", frames, out_path,
                 objects, paths_path);
    return 0;
}

int run_atmos_encode(std::string_view in_path, std::string_view out_path,
                     std::uint32_t bitrate, std::uint32_t objects,
                     const Options& meta, std::string_view paths_path) {
    // The same streaming-vs-whole-file split as run_encode - see its
    // comment. This command has no dual-mono merge, so only stdin and
    // dialnorm=auto (whole-programme BS.1770) force the whole-file read.
    ac3::io::WavStreamReader stream_in;
    const bool streaming = !is_stdio_path(in_path) && !meta.p.measure_dialnorm &&
                           stream_in.open(std::string{in_path}).has_value();
    std::expected<ac3::io::WavData, ac3::io::WavError> wav =
        std::unexpected(ac3::io::WavError::kCannotOpen);
    if (!streaming) {
        wav = read_wav_arg(in_path);
        if (!wav) {
            std::println(stderr, "error: {}: {}", in_path, ac3::io::describe(wav.error()));
            return 1;
        }
    }
    const std::uint32_t src_rate = streaming ? stream_in.sample_rate() : wav->sample_rate;
    const std::size_t src_channels =
        streaming ? stream_in.channels() : wav->channels.size();
    const auto sr = wav_sample_rate(src_rate, "E-AC-3", true);
    if (!sr) {
        return 1;
    }
    // One object per source channel unless told otherwise; more objects than
    // the file has channels would leave some carrying nothing.
    const auto count = objects == 0 ? src_channels
                                    : std::min<std::size_t>(objects, src_channels);
    if (count < 1 || count > 15) {
        std::println(stderr,
                     "error: 1 to 15 objects (the bed's LFE is the 16th, and TS 103 420 "
                     "§8.3.2.2 caps the total at 16); this file has {} channels",
                     src_channels);
        return 1;
    }

    // status_stream(out_path): stderr instead of stdout when out_path is "-"
    // - the E-AC-3 bytes this function writes below already own stdout in
    // that case, and no human-readable report (the dialnorm=auto measurement
    // just below included) may land in the middle of them.
    const auto status = status_stream(out_path);
    int dialnorm = meta.p.dialnorm;
    if (meta.p.measure_dialnorm) {
        const auto layout = ac3::io::ac3_layout_for(src_channels);
        const auto measured = layout
                                  ? measured_dialnorm(*wav, *sr, layout->acmod, layout->lfe, status)
                                  : std::nullopt;
        if (!measured) {
            std::println(stderr, "error: cannot measure loudness for this file; "
                                 "pass dialnorm=<1..31> explicitly");
            return 1;
        }
        dialnorm = *measured;
    }

    ac3::oba::AtmosEncoder encoder{
        {.sample_rate = *sr, .bitrate_kbps = bitrate, .dialnorm = dialnorm, .num_bands_idx = 4,
         .fast_mdct = meta.fast_mdct},
        static_cast<int>(count)};

    // Objects that reach the bed by the same route are exactly the ones JOC
    // cannot pull apart again, so the source's channels are spread across the
    // room rather than stacked at one point. A channel that already has a
    // direction keeps it; the rest fan out evenly.
    std::vector<ac3::oba::ObjectPlacement> placement(count);
    const auto layout = ac3::io::ac3_layout_for(src_channels);
    for (std::size_t i = 0; i < count; ++i) {
        double azimuth = 0.0;
        if (layout) {
            // wav_index maps a coded channel to a WAV one; this needs the
            // inverse, so the channel is found rather than indexed.
            for (std::size_t k = 0; k < layout->wav_index.size(); ++k) {
                if (layout->wav_index[k] != i) {
                    continue;
                }
                azimuth = ac3::analysis::channel_azimuth_deg(layout->acmod, layout->lfe,
                                                             static_cast<int>(k))
                              .value_or(0.0);
            }
        } else {
            azimuth = 360.0 * static_cast<double>(i) / static_cast<double>(count);
        }
        const double radians = azimuth * std::numbers::pi / 180.0;
        placement[i] = {.position = {.x = 0.5 - 0.5 * std::sin(radians),
                                     .y = 0.5 - 0.5 * std::cos(radians),
                                     .z = 0.0},
                        // Every object is panned into the same five channels,
                        // so their contributions add there. The same
                        // inverse-root law 'atmos' and the GUI use, so a file
                        // encoded either way comes out at the same level.
                        .gain = 0.7 / std::sqrt(static_cast<double>(count)),
                        .lfe_send = 0.0};
    }

    // An authored keyframe file (same format/addressing as atmos-path, object
    // index == this WAV channel index) drives motion instead of the static
    // placement above; empty (the default) leaves that placement reused
    // unchanged every frame, exactly as before this argument existed - see
    // the per-frame loop below.
    std::optional<std::vector<ac3::oba::ObjectPath>> paths;
    if (!paths_path.empty()) {
        const auto parsed = parse_path_file(paths_path);
        if (!parsed) {
            return 1;
        }
        paths.emplace();
        paths->reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            if (i < parsed->size() && !(*parsed)[i].empty()) {
                auto created = ac3::oba::KeyframePath::create((*parsed)[i]);
                if (!created) {
                    std::println(stderr, "error: object {} has two keyframes at the same time_s",
                                 i);
                    return 1;
                }
                paths->emplace_back(std::move(*created));
                continue;
            }
            // Not mentioned in the file: keep exactly the placement this
            // object has today, just re-expressed as a (never-moving) path.
            auto fallback = ac3::oba::KeyframePath::create({{.time_s = 0.0,
                                                              .position = placement[i].position,
                                                              .gain = placement[i].gain,
                                                              .lfe_send = placement[i].lfe_send}});
            paths->emplace_back(std::move(*fallback));
        }
    }

    ac3::analysis::LevelMeter meter{ac3::Acmod::k3_2, true, src_rate};
    const std::size_t total =
        streaming ? static_cast<std::size_t>(stream_in.frame_count()) : wav->frame_count();
    std::vector<std::vector<float>> block(count, std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> views(count);
    std::vector<std::span<const float>> metered(6);
    // Streamed out as encoded - except under sign-objects, where the frames
    // defer inside the sink because the signing pass below rewrites every
    // one of them after this loop.
    EncodedStreamSink out_sink;
    if (!out_sink.open(out_path, meta.keep_partial, /*defer=*/meta.sign_objects)) {
        return 1;
    }
    // Streaming reads every file channel (read_planar's contract), but only
    // the first `count` become objects - the extras land in one shared
    // discard buffer whose contents nothing reads.
    std::vector<float> stream_discard(streaming ? ac3::kSamplesPerFrame : 0);
    std::vector<std::span<float>> stream_dst(streaming ? src_channels : 0);

    for (std::size_t start = 0; start < total; start += ac3::kSamplesPerFrame) {
        const auto valid = std::min<std::size_t>(ac3::kSamplesPerFrame, total - start);
        if (streaming) {
            for (std::size_t ch = 0; ch < src_channels; ++ch) {
                stream_dst[ch] = ch < count ? std::span{block[ch]}.first(valid)
                                            : std::span{stream_discard}.first(valid);
            }
            const auto got = stream_in.read_planar(stream_dst, valid);
            if (!got || *got != valid) {
                std::println(stderr, "error: {}: {}", in_path,
                             ac3::io::describe(got ? ac3::io::WavError::kTruncated
                                                   : got.error()));
                out_sink.abort();
                return 1;
            }
            for (std::size_t ch = 0; ch < count; ++ch) {
                // The tail frame zero-pads past the file's end, exactly as
                // the whole-file loop below writes 0.0f there.
                std::fill(block[ch].begin() + static_cast<std::ptrdiff_t>(valid),
                          block[ch].end(), 0.0f);
                views[ch] = block[ch];
            }
        } else {
            for (std::size_t ch = 0; ch < count; ++ch) {
                for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
                    const std::size_t at = start + static_cast<std::size_t>(i);
                    block[ch][static_cast<std::size_t>(i)] =
                        at < total ? wav->channels[ch][at] : 0.0f;
                }
                views[ch] = block[ch];
            }
        }
        // With paths_path, the object placement moves - evaluated at the
        // frame's END time, the same convention run_atmos_path and the GUI's
        // encodeObjects use. Without it, every frame reuses the one static
        // placement computed above, byte-identical to before this argument
        // existed.
        auto unit = paths ? encoder.encode_frame(
                                views, ac3::oba::evaluate_placements(
                                           *paths, static_cast<double>(start + ac3::kSamplesPerFrame) /
                                                       static_cast<double>(src_rate)))
                          : encoder.encode_frame(views, placement);
        if (!unit) {
            std::println(stderr,
                         "error: cannot encode {} objects at {} kbps — the metadata and the "
                         "mantissas share one frame, so try a higher bit rate",
                         count, bitrate);
            out_sink.abort();
            return 1;
        }
        // The bed exists only once the frame is encoded, so it is metered
        // afterwards - and it is the bed, not the source, that a legacy
        // decoder plays.
        for (std::size_t ch = 0; ch < metered.size(); ++ch) {
            metered[ch] = std::span{encoder.bed()[ch]}.first(valid);
        }
        meter.process(metered);
        if (!out_sink.push(std::move(unit->bytes))) {
            out_sink.abort();
            return 1;
        }
    }
    // Optional object signing, same as 'atmos' - see the comments at its
    // call site there, the key-failure plain return included. Goes through
    // status_stream() like the report below: with out_path == "-" the
    // E-AC-3 bytes about to be written own stdout.
    const auto signed_count = apply_object_signing(out_sink.deferred(), meta);
    if (!signed_count) {
        return 1;
    }
    if (*signed_count > 0) {
        std::println(status_stream(out_path),
                     "  signed {} frames' EMDF object container with the supplied key",
                     *signed_count);
    }
    if (!out_sink.close()) {
        return 1;
    }
    std::println(status, "encoded {} E-AC-3 access units ({} kbps, {} Hz) to {}",
                out_sink.frames(), bitrate, src_rate, out_path);
    std::println(status,
                 "  {} objects from {} source channels + the bed's LFE = {} objects, "
                 "JOC over a 5.1 downmix",
                 count, src_channels, ac3::oba::object_count(encoder.program()));
    print_channel_summary(meter, status);
    return 0;
}

int run_atmos_adm(std::string_view in_path, std::string_view out_path, std::uint32_t bitrate,
                  const Options& meta, std::string_view programme_id) {
    // No fixed source layout to measure a pre-encode loudness figure against the way
    // atmos-encode's WAV input has (ac3::io::ac3_layout_for) - an ADM document's channels are an
    // arbitrary mix of bed speaker feeds and dynamic objects, not one of the handful of layouts
    // that function maps. Refusing clearly beats silently keeping the fixed default dialnorm:
    // "a silently ignored metadata flag looks exactly like metadata that did not work" (see
    // parse_options's own comment above).
    if (meta.p.measure_dialnorm) {
        std::println(stderr,
                     "error: dialnorm=auto is not supported by atmos-adm - an ADM document's bed/"
                     "object channels have no single fixed layout to measure loudness against the "
                     "way atmos-encode's WAV input does; pass dialnorm=<1..31> explicitly");
        return 1;
    }

    auto source = ac3cli::load_adm_atmos_source(in_path, programme_id);
    if (!source) {
        std::println(stderr, "error: {}: {}", in_path, source.error());
        return 1;
    }

    const auto sr = wav_sample_rate(source->sample_rate, "E-AC-3", true);
    if (!sr) {
        return 1;
    }

    const auto count = source->channel_count();
    if (count < 1 || count > 15) {
        std::println(stderr,
                     "error: 1 to 15 bed/object channels (the bed's LFE is the 16th, and TS 103 "
                     "420 §8.3.2.2 caps the total at 16); {} resolved {} channel(s)",
                     in_path, count);
        return 1;
    }

    ac3::oba::AtmosEncoder encoder{
        {.sample_rate = *sr, .bitrate_kbps = bitrate, .dialnorm = meta.p.dialnorm,
         .num_bands_idx = 4, .fast_mdct = meta.fast_mdct},
        static_cast<int>(count)};

    // Metered the same way run_atmos_encode meters its own bed: 3/2 + LFE is AtmosEncoder's own
    // fixed bed layout regardless of how many dynamic objects/bed feeds fed it.
    ac3::analysis::LevelMeter meter{ac3::Acmod::k3_2, true, source->sample_rate};
    const std::size_t total = source->pcm.empty() ? 0 : source->pcm.front().size();
    std::vector<std::vector<float>> block(count, std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> views(count);
    std::vector<std::span<const float>> metered(6);
    // Streamed out as encoded - no sign-objects on this command, so no
    // defer case either.
    EncodedStreamSink out_sink;
    if (!out_sink.open(out_path, meta.keep_partial)) {
        return 1;
    }

    for (std::size_t start = 0; start < total; start += ac3::kSamplesPerFrame) {
        const auto valid = std::min<std::size_t>(ac3::kSamplesPerFrame, total - start);
        for (std::size_t ch = 0; ch < count; ++ch) {
            for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
                const std::size_t at = start + static_cast<std::size_t>(i);
                block[ch][static_cast<std::size_t>(i)] =
                    at < source->pcm[ch].size() ? source->pcm[ch][at] : 0.0f;
            }
            views[ch] = block[ch];
        }
        // Evaluated at the frame's END time, the same convention run_atmos_path/run_atmos_encode
        // use.
        const auto placement = ac3::oba::evaluate_placements(
            source->paths, static_cast<double>(start + ac3::kSamplesPerFrame) /
                                static_cast<double>(source->sample_rate));
        auto unit = encoder.encode_frame(views, placement);
        if (!unit) {
            std::println(stderr,
                         "error: cannot encode {} channels at {} kbps — the metadata and the "
                         "mantissas share one frame, so try a higher bit rate",
                         count, bitrate);
            out_sink.abort();
            return 1;
        }
        // The bed exists only once the frame is encoded, so it is metered afterwards - and it is
        // the bed, not the source, that a legacy decoder plays.
        for (std::size_t ch = 0; ch < metered.size(); ++ch) {
            metered[ch] = std::span{encoder.bed()[ch]}.first(valid);
        }
        meter.process(metered);
        if (!out_sink.push(std::move(unit->bytes))) {
            out_sink.abort();
            return 1;
        }
    }
    if (!out_sink.close()) {
        return 1;
    }

    std::size_t bed_count = 0;
    for (const bool is_bed : source->is_bed) {
        bed_count += is_bed ? 1 : 0;
    }
    // See run_encode's identical status_stream() comment: out_path == "-" means the E-AC-3 bytes
    // just written own stdout, so this report goes to stderr instead.
    const auto status = status_stream(out_path);
    std::println(status, "encoded {} E-AC-3 access units ({} kbps, {} Hz) from {} to {}",
                out_sink.frames(), bitrate, source->sample_rate, in_path, out_path);
    std::println(status,
                 "  {} bed speaker feed(s) + {} dynamic object(s) + the bed's LFE = {} objects, "
                 "JOC over a 5.1 downmix",
                 bed_count, count - bed_count, ac3::oba::object_count(encoder.program()));
    print_channel_summary(meter, status);
    return 0;
}

}  // namespace ac3cli::commands
