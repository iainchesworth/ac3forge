#include "decode.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "../support.hpp"
#include "ac3/analysis/levels.hpp"
#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/plan.hpp"
#include "ac3/io/wav.hpp"
#include "ac3/meta/drc.hpp"
#include "ac3/oba/oamd.hpp"

namespace ac3cli::commands {

namespace {

namespace plan = ac3::plan;

// Reports the object layer (if any) an E-AC-3 decode found - the decode-side
// mirror of run_atmos_encode's own "{N} dynamic objects + the bed's LFE = {M}
// objects" line. Shared between run_decode_eac3's dual-mono and ordinary
// return paths, even though this project's own AtmosEncoder never emits dual
// mono alongside an object container. The object WAVs themselves are
// streamed out by per-object sinks as the decode runs (run_decode_eac3's
// append_objects) - by the time this prints, the files are already closed;
// this only says what happened.
int report_decoded_objects(FILE* status, const std::optional<ac3::oba::DecodedProgram>& metadata,
                           bool have_object_audio, std::size_t objects_written,
                           std::string_view objects_dir) {
    if (metadata) {
        std::println(status, "  {} dynamic objects + the bed's LFE = {} objects, OAMD present{}",
                     metadata->objects.size(), ac3::oba::object_count(metadata->program),
                     have_object_audio ? ", JOC audio reconstructed"
                                       : " (JOC audio not reconstructed)");
    }
    if (objects_dir.empty()) {
        return 0;
    }
    if (objects_written == 0) {
        std::println(stderr,
                     "warning: objects_dir given but there is no reconstructed object audio to "
                     "export");
        return 0;
    }
    std::println(status, "  wrote {} object WAV(s) to {}", objects_written, objects_dir);
    return 0;
}

// The dynrng/compr half of run_decode's own status report (main.cpp, further
// down), factored out so run_decode_eac3 can report the same two figures -
// range actually carried, and whether drc=/heavy asked for them to be
// applied - without duplicating run_decode's own dialnorm-anchored
// indentation, which this command's report has no dialnorm line to anchor to.
void print_drc_summary(FILE* status, double dynrng_min_db, double dynrng_max_db,
                       double compr_min_db, double compr_max_db, std::size_t compr_frames,
                       const ac3cli::Options& meta) {
    std::println(status, "  dynrng {:+.2f} .. {:+.2f} dB{}", dynrng_min_db, dynrng_max_db,
                 meta.drc_scale != 0.0 ? std::format(", applied at scale {}", meta.drc_scale)
                                       : ", not applied");
    if (compr_frames > 0) {
        std::println(status, "  compr  {:+.2f} .. {:+.2f} dB over {} access units{}",
                     compr_min_db, compr_max_db, compr_frames,
                     meta.p.heavy ? ", applied" : ", not applied");
    } else {
        std::println(status, "  compr  absent");
    }
}

int run_decode_eac3(std::span<const std::byte> stream, std::string_view out_path,
                     const ac3cli::Options& meta, std::string_view objects_dir) {
    // Access units, not syncframes: a dependent substream is only meaningful
    // alongside the independent one it extends, and the two are rendered
    // together into one set of speaker feeds.
    const auto units = ac3::split_access_units(stream);
    if (!units) {
        std::println(stderr, "error: stream framing failed (code {})",
                     static_cast<int>(units.error()));
        return 1;
    }
    ac3::Eac3Decoder decoder{
        {.drc_scale = meta.drc_scale,
         .fast_imdct = meta.fast_imdct,
         .heavy_compression = meta.p.heavy.has_value()}};
    // The decoded programme goes out through the sink as units decode - the
    // sink's per-slot carry absorbs the one place slots advance unevenly
    // (the transient-pre-noise flush below).
    PlanarWavSink sink;
    std::size_t sink_slots = 0;
    const auto open_sink = [&](const ac3::DecodedAccessUnit& unit,
                               std::size_t slots) -> bool {
        sink_slots = slots;
        // Dual mono has no Table E2.5 location to order by, so Ch1 and Ch2
        // go out in coded order - the same identity the whole-buffer write
        // fell back to. Everyone else gets the WAV speaker order the encode
        // side reads a file in.
        std::vector<std::size_t> order;
        if (unit.acmod != ac3::Acmod::kDualMono) {
            order = plan::wav_order(std::span{unit.layout.items}.first(
                static_cast<std::size_t>(unit.layout.count)));
        }
        if (!sink.open(out_path, sample_rate_hz(unit.sample_rate), slots, order)) {
            std::println(stderr, "error: cannot open {} for writing", out_path);
            return false;
        }
        return true;
    };
    // JOC's reconstructed per-object audio - parallel to
    // first.object_metadata->objects (same index, same object). With no
    // objects_dir nothing keeps it: only the fact that some arrived matters
    // to the report. With one, each object streams to its own mono WAV. An
    // access unit whose object_audio size doesn't match the sinks is
    // skipped rather than resized into: DecodedSubstream's own comment
    // documents this as reachable (a program-shape mismatch JOC's ordering
    // can't be lined up against), not something worth failing the whole
    // decode over.
    bool have_object_audio = false;
    std::vector<PlanarWavSink> object_sinks;
    const auto abort_all = [&] {
        sink.abort();
        for (auto& object_sink : object_sinks) {
            object_sink.abort();
        }
    };
    const auto append_objects = [&](const std::vector<std::vector<float>>& object_audio,
                                    std::uint32_t sample_rate) -> bool {
        if (object_audio.empty()) {
            return true;
        }
        have_object_audio = true;
        if (objects_dir.empty()) {
            return true;
        }
        if (object_sinks.empty()) {
            std::error_code ec;
            const std::filesystem::path dir{std::string{objects_dir}};
            std::filesystem::create_directories(dir, ec);
            if (ec) {
                std::println(stderr, "error: cannot create directory {} ({})", objects_dir,
                             ec.message());
                return false;
            }
            object_sinks.resize(object_audio.size());
            for (std::size_t i = 0; i < object_sinks.size(); ++i) {
                const auto object_path = dir / std::format("object_{:02}.wav", i);
                if (!object_sinks[i].open(object_path.string(), sample_rate, 1, {})) {
                    std::println(stderr, "error: cannot open {} for writing",
                                 object_path.string());
                    return false;
                }
            }
        }
        if (object_audio.size() != object_sinks.size()) {
            return true;  // shape mismatch: skipped, same as the old append
        }
        for (std::size_t i = 0; i < object_sinks.size(); ++i) {
            if (!object_sinks[i].append(0, object_audio[i])) {
                std::println(stderr, "error: cannot write object audio under {}", objects_dir);
                return false;
            }
        }
        return true;
    };
    ac3::DecodedAccessUnit first{};
    // What the independent (bed) substream actually carried, reported whether
    // or not it was applied - same convention as run_decode's own dynrng_min_db/
    // dynrng_max_db/compr_min_db/compr_max_db above, except both are seeded
    // from the first real word rather than from 0.0: a stream whose transmitted
    // dynrng/compr never happens to cross exactly unity would otherwise have
    // its true min or max silently clamped to 0 dB by the seed itself.
    double dynrng_min_db = 0.0;
    double dynrng_max_db = 0.0;
    std::size_t dynrng_words = 0;
    double compr_min_db = 0.0;
    double compr_max_db = 0.0;
    std::size_t compr_frames = 0;
    // numblkscod bounds how many of `dynrng`'s kBlocksPerFrame entries are
    // real: E-AC-3 (unlike AC-3) can code as few as one block per syncframe,
    // and the rest of the fixed-size array is never written (DecodedSubstream::
    // dynrng's own comment) - folding those unwritten, always-unity entries in
    // here would understate the true range for any such stream.
    const auto track_metadata = [&](const std::array<std::uint8_t, ac3::kBlocksPerFrame>& dynrng,
                                    int numblkscod, std::optional<std::uint8_t> compr) {
        const auto nblks =
            static_cast<std::size_t>(ac3::eac3::blocks_per_syncframe(numblkscod));
        for (std::size_t i = 0; i < nblks; ++i) {
            const double db = ac3::meta::to_db(ac3::meta::dynrng_gain(dynrng[i]));
            dynrng_min_db = dynrng_words == 0 ? db : std::min(dynrng_min_db, db);
            dynrng_max_db = dynrng_words == 0 ? db : std::max(dynrng_max_db, db);
            ++dynrng_words;
        }
        if (compr) {
            const double db = ac3::meta::to_db(ac3::meta::compr_gain(*compr));
            compr_min_db = compr_frames == 0 ? db : std::min(compr_min_db, db);
            compr_max_db = compr_frames == 0 ? db : std::max(compr_max_db, db);
            ++compr_frames;
        }
    };
    for (const auto& unit : *units) {
        const auto decoded = decoder.decode_access_unit(unit);
        if (!decoded) {
            std::println(stderr, "error: decode failed (code {})",
                         static_cast<int>(decoded.error()));
            abort_all();
            return 1;
        }
        if (!decoded->has_value()) {
            // §3.7: this access unit's frame(s) are being held back pending
            // transient pre-noise processing (Eac3Decoder::decode_access_unit's
            // own doc comment) - nothing new to append yet, not an error.
            continue;
        }
        const auto& out = **decoded;
        if (!sink.is_open()) {
            first = out;
            if (!open_sink(first, out.channels.size())) {
                return 1;
            }
        }
        track_metadata(out.dynrng, out.numblkscod, out.compr);
        for (std::size_t ch = 0; ch < out.channels.size(); ++ch) {
            if (!sink.append(ch, out.channels[ch])) {
                std::println(stderr, "error: cannot write to {}", out_path);
                abort_all();
                return 1;
            }
        }
        if (!append_objects(out.object_audio, sample_rate_hz(first.sample_rate))) {
            abort_all();
            return 1;
        }
    }
    // Whatever transient pre-noise processing was still holding back at
    // end-of-stream. flush() returns raw per-substream results rather than
    // assembled access units (see its own doc comment) - placed at the SAME
    // pcm slot decode_access_unit's own §E3.8.2 assembly would have used
    // (via location_map()), not assumed to already sit at that slot: a lone
    // independent substream's coded order happens to agree with pcm's, but
    // a dependent carrying only its own smaller channel set does not, and
    // naively appending it by coded index corrupts already-established
    // channels (e.g. a bed's L/R) with a dependent's height audio instead.
    const auto flushed = decoder.flush();
    if (!flushed.empty()) {
        // §7.7 words are meaningful at this report's level only from the
        // independent (bed) substream - same convention as
        // DecodedAccessUnit::dynrng/compr above; a dependent flushed here
        // (only possible when transient pre-noise processing has left
        // substreams of one access unit desynchronised at end-of-stream) is
        // never the figure this report promises.
        for (const auto& substream : flushed) {
            if (substream.strmtyp == ac3::eac3::StreamType::kIndependent) {
                track_metadata(substream.dynrng, substream.numblkscod, substream.compr);
            }
        }
        const bool dual_mono = sink.is_open() ? first.acmod == ac3::Acmod::kDualMono
                                              : flushed.front().acmod == ac3::Acmod::kDualMono;
        if (dual_mono) {
            // No Table E2.5 location to place by - dual mono is always a
            // lone substream with no dependents and no spatial layout
            // (decode_access_unit's own comment) - so its channels go
            // straight out in coded order, same as decode_access_unit.
            for (const auto& substream : flushed) {
                if (!sink.is_open()) {
                    first.acmod = ac3::Acmod::kDualMono;
                    first.sample_rate = substream.sample_rate;
                    first.dialnorm = substream.dialnorm;
                    first.substream_count = 1;
                    first.object_metadata = substream.object_metadata;
                    if (!open_sink(first, substream.channels.size())) {
                        return 1;
                    }
                }
                for (std::size_t ch = 0; ch < substream.channels.size(); ++ch) {
                    if (!sink.append(ch, substream.channels[ch])) {
                        std::println(stderr, "error: cannot write to {}", out_path);
                        abort_all();
                        return 1;
                    }
                }
            }
        } else {
            if (!sink.is_open()) {
                // No access unit ever completed - synthesize the program's
                // layout by unioning every flushed substream's own
                // locations, exactly like decode_access_unit's own §E3.8.2
                // assembly.
                std::uint16_t occupied = 0;
                for (const auto& substream : flushed) {
                    occupied = static_cast<std::uint16_t>(occupied | substream.location_map());
                }
                ac3::DecodedAccessUnit synthesized;
                synthesized.sample_rate = flushed.front().sample_rate;
                synthesized.acmod = flushed.front().acmod;
                synthesized.dialnorm = flushed.front().dialnorm;
                synthesized.substream_count = static_cast<int>(flushed.size());
                synthesized.layout = ac3::eac3::chanmap::expand(occupied);
                // Object audio only ever rides in the bed (the independent
                // substream) - see DecodedAccessUnit::object_metadata's own
                // comment - so at most one flushed substream carries it.
                for (const auto& substream : flushed) {
                    if (substream.object_metadata) {
                        synthesized.object_metadata = substream.object_metadata;
                        break;
                    }
                }
                first = synthesized;
                if (!open_sink(first, static_cast<std::size_t>(first.layout.count))) {
                    return 1;
                }
            }
            // §E3.8.2 placement: each flushed substream's own channels land
            // at whichever slot their Table E2.5 location occupies in
            // `first.layout`, mirroring decode_access_unit's own assembly
            // loop. Different substreams may append different lengths to
            // different slots here; the sink's per-slot carry absorbs it.
            for (const auto& substream : flushed) {
                const auto locations = ac3::eac3::chanmap::expand(substream.location_map());
                for (int i = 0; i < locations.count; ++i) {
                    const int slot = first.layout.index_of(locations[i]);
                    if (slot < 0) {
                        continue;
                    }
                    if (!sink.append(static_cast<std::size_t>(slot),
                                     substream.channels[static_cast<std::size_t>(i)])) {
                        std::println(stderr, "error: cannot write to {}", out_path);
                        abort_all();
                        return 1;
                    }
                }
            }
        }
        // JOC's reconstructed per-object audio, streamed the same way the
        // main access-unit loop's is - see append_objects for why a size
        // mismatch is skipped rather than resized into.
        for (const auto& substream : flushed) {
            if (!append_objects(substream.object_audio,
                                sample_rate_hz(substream.sample_rate))) {
                abort_all();
                return 1;
            }
        }
    }
    if (!sink.is_open()) {
        std::println(stderr, "error: no access units");
        return 1;
    }
    // Dual mono has no Table E2.5 location to order by - decode_access_unit
    // leaves `layout` empty for exactly this case - so Ch1 and Ch2 go out in
    // coded order, the same identity write_wav_f32 falls back to itself.
    // status_stream(out_path): stderr instead of stdout when out_path is "-"
    // - the WAV bytes the write below produces already own stdout in that
    // case, and this report must not land in the middle of them.
    const auto status = status_stream(out_path);
    const auto written = sink.close();
    if (!written) {
        std::println(stderr, "error: {}", ac3::io::describe(written.error()));
        return 1;
    }
    std::size_t objects_written = 0;
    for (auto& object_sink : object_sinks) {
        if (const auto closed = object_sink.close(); !closed) {
            std::println(stderr, "error: {}", ac3::io::describe(closed.error()));
            return 1;
        }
        ++objects_written;
    }
    if (first.acmod == ac3::Acmod::kDualMono) {
        std::println(status, "decoded {} E-AC-3 access units ({} substreams each) -> {}",
                     units->size(), first.substream_count, out_path);
        std::println(status,
                     "  {} channels, {} Hz: Ch1 Ch2 (1+1 dual mono - two programmes, not a "
                     "soundfield)",
                     sink_slots, sample_rate_hz(first.sample_rate));
        print_drc_summary(status, dynrng_min_db, dynrng_max_db, compr_min_db, compr_max_db,
                          compr_frames, meta);
        return report_decoded_objects(status, first.object_metadata, have_object_audio,
                                      objects_written, objects_dir);
    }
    // The same WAV speaker order the encode side reads a file in, so a stream
    // decoded here and re-encoded lands every channel back where it started -
    // recomputed here only for the speaker-name report; the sink applied it.
    const auto map = plan::wav_order(
        std::span{first.layout.items}.first(static_cast<std::size_t>(first.layout.count)));
    std::string speakers;
    for (const auto index : map) {
        speakers += ac3::eac3::chanmap::name(first.layout[static_cast<int>(index)]);
        speakers += ' ';
    }
    std::println(status, "decoded {} E-AC-3 access units ({} substreams each) -> {}",
                 units->size(), first.substream_count, out_path);
    std::println(status, "  {} channels, {} Hz: {}", map.size(), sample_rate_hz(first.sample_rate),
                 speakers);
    print_drc_summary(status, dynrng_min_db, dynrng_max_db, compr_min_db, compr_max_db,
                      compr_frames, meta);
    return report_decoded_objects(status, first.object_metadata, have_object_audio,
                                  objects_written, objects_dir);
}

}  // namespace

int run_decode(std::string_view in_path, std::string_view out_path, const ac3cli::Options& meta,
               std::string_view objects_dir) {
    const auto stream = read_all(in_path);
    if (stream.empty()) {
        std::println(stderr, "error: cannot read {}", in_path);
        return 1;
    }
    if (!apply_object_verification(stream, meta)) {
        return 1;
    }
    // bsid at bit 40 says which syntax this is, before either is assumed.
    // spdif and play branch on it the same way now that both packers handle
    // E-AC-3 (Eac3BurstPacker alongside AC-3's wrap_frame).
    const auto bsid = ac3::stream_bsid(stream);
    if (!bsid) {
        std::println(stderr, "error: {} is too short to hold a syncframe", in_path);
        return 1;
    }
    if (*bsid > 8) {
        return run_decode_eac3(stream, out_path, meta, objects_dir);
    }
    if (!objects_dir.empty()) {
        std::println(stderr,
                     "warning: objects_dir given but {} is plain AC-3 - it has no object layer",
                     in_path);
    }
    const auto frames = ac3::split_frames(stream);
    if (!frames) {
        std::println(stderr, "error: {}: {}", in_path, ac3::describe(frames.error()));
        return 1;
    }
    ac3::FrameDecoder decoder{
        {.drc_scale = meta.drc_scale,
         .fast_imdct = meta.fast_imdct,
         .heavy_compression = meta.p.heavy.has_value()}};
    PlanarWavSink sink;
    std::optional<ac3::analysis::LevelMeter> meter;
    ac3::DecodedFrame first{};
    bool have_first = false;
    // What the stream actually carried, reported whether or not it was applied.
    double dynrng_min_db = 0.0;
    double dynrng_max_db = 0.0;
    std::size_t dynrng_words = 0;
    double compr_min_db = 0.0;
    double compr_max_db = 0.0;
    std::size_t compr_frames = 0;
    for (const auto& frame : *frames) {
        const auto decoded = decoder.decode_frame(frame);
        if (!decoded) {
            std::println(stderr, "error: {}: {}", in_path, ac3::describe(decoded.error()));
            sink.abort();
            return 1;
        }
        for (const auto word : decoded->dynrng) {
            const double db = ac3::meta::to_db(ac3::meta::dynrng_gain(word));
            dynrng_min_db = dynrng_words == 0 ? db : std::min(dynrng_min_db, db);
            dynrng_max_db = dynrng_words == 0 ? db : std::max(dynrng_max_db, db);
            ++dynrng_words;
        }
        if (decoded->compr) {
            const double db = ac3::meta::to_db(ac3::meta::compr_gain(*decoded->compr));
            compr_min_db = compr_frames == 0 ? db : std::min(compr_min_db, db);
            compr_max_db = compr_frames == 0 ? db : std::max(compr_max_db, db);
            ++compr_frames;
        }
        if (!have_first) {
            first = *decoded;
            // The channel permutation the whole-buffer write used to apply
            // at the end is fixed from the first frame's layout - the same
            // values, just needed up front now that samples leave as they
            // decode.
            if (!sink.open(out_path, sample_rate_hz(decoded->sample_rate),
                           decoded->channels.size(),
                           ac3::io::wav_channel_order(decoded->acmod, decoded->lfe))) {
                std::println(stderr, "error: cannot open {} for writing", out_path);
                return 1;
            }
            meter.emplace(decoded->acmod, decoded->lfe, sample_rate_hz(decoded->sample_rate));
            have_first = true;
        }
        std::vector<std::span<const float>> views;
        views.reserve(decoded->channels.size());
        for (std::size_t ch = 0; ch < decoded->channels.size(); ++ch) {
            if (!sink.append(ch, decoded->channels[ch])) {
                std::println(stderr, "error: cannot write to {}", out_path);
                sink.abort();
                return 1;
            }
            views.emplace_back(decoded->channels[ch]);
        }
        // have_first gates meter.emplace() a few lines up, in this same
        // iteration on the first pass and an earlier one on every pass
        // after, so meter is always engaged by the time this line runs.
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        meter->process(views);
    }
    if (!have_first) {
        std::println(stderr, "error: no frames");
        return 1;
    }
    const auto written = sink.close();
    if (!written) {
        std::println(stderr, "error: {}", ac3::io::describe(written.error()));
        return 1;
    }
    // status_stream(out_path): stderr instead of stdout when out_path is "-"
    // - the WAV bytes just written above already own stdout in that case,
    // and this report must not land in the middle of them.
    const auto status = status_stream(out_path);
    std::println(status, "decoded {} frames -> {} ({}, {} Hz)", frames->size(), out_path,
                 ac3::analysis::layout_name(first.acmod, first.lfe),
                 sample_rate_hz(first.sample_rate));
    std::println(status, "metadata: dialnorm {} (dialogue at -{} dBFS)", first.dialnorm,
                 first.dialnorm);
    if (first.dialnorm2) {
        std::println(status, "          dialnorm2 {} (Ch2, dialogue at -{} dBFS){}",
                     *first.dialnorm2, *first.dialnorm2, first.compr2 ? ", compr2 present" : "");
    }
    std::println(status, "          dynrng {:+.2f} .. {:+.2f} dB{}", dynrng_min_db, dynrng_max_db,
                 meta.drc_scale != 0.0 ? std::format(", applied at scale {}", meta.drc_scale)
                                       : ", not applied");
    if (compr_frames > 0) {
        std::println(status, "          compr  {:+.2f} .. {:+.2f} dB over {} frames{}",
                     compr_min_db, compr_max_db, compr_frames,
                     meta.p.heavy ? ", applied" : ", not applied");
    } else {
        std::println(status, "          compr  absent");
    }
    // The have_first check above already returned if the frame loop never
    // ran, and it is that same loop's first iteration that emplaces meter.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    print_channel_summary(*meter, status);
    return 0;
}

}  // namespace ac3cli::commands
