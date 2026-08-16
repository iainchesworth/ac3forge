#include "ac3adm/ac3adm.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <istream>
#include <iterator>
#include <random>
#include <sstream>
#include <utility>

#include <adm/errors.hpp>
#include <adm/parse.hpp>
#include <bw64/bw64.hpp>

#include "adm_model.hpp"

// Every `bw64::`/`adm::` symbol below is a vendored third-party library
// (libbw64/libadm respectively, see src/ac3adm/CMakeLists.txt); every
// `ac3adm::` symbol is this module's own. Both libraries report failure by
// throwing std::runtime_error (or, for libadm's XML/schema errors, the
// adm::error::AdmException hierarchy) - this project's own convention is
// std::expected for stream-level/recoverable failure (CONTRIBUTING.md), so
// every call into either library is wrapped here at this one boundary and
// translated into AdmError rather than letting an exception escape this
// module's public API.

namespace ac3adm {

std::string_view describe(AdmError error) {
    switch (error) {
        case AdmError::kCannotOpen: return "cannot open file";
        case AdmError::kNotRiff: return "not a RIFF/RF64/BW64 WAVE file";
        case AdmError::kMissingFmt: return "missing fmt chunk";
        case AdmError::kMissingData: return "missing data chunk";
        case AdmError::kUnsupportedFormat: return "unsupported audio format";
        case AdmError::kMalformedXml: return "axml chunk is not well-formed XML";
        case AdmError::kMalformedAdm: return "axml chunk XML is not a valid ADM document";
        case AdmError::kOther: return "unexpected failure reading the BW64/ADM file";
    }
    return "unknown error";
}

namespace {

// A unique path under the system temp directory for parse_bw64(std::istream&)'s spool file (see
// its own comment below). NOT derived from the istream's own address: an earlier version did
// exactly that (reinterpret_cast<std::uintptr_t>(&in)), which looked unique enough in a single
// process but is not - Windows does not vary a given call frame's stack address between separate
// launches of the same binary much, if at all, so two of this project's own ctest entries
// (each ac3tests.exe test case is its own process, and ctest -j runs many of them concurrently)
// landed on the exact same temp filename and raced on it, one process's write clobbering the
// other's read mid-parse. Caught via a real, intermittent ctest failure under -j8 that a single
// direct run of the same test could not reproduce - the actual symptom (not a hypothesis) that
// justified this fix. A high-resolution clock reading XORed with a random_device draw and a
// monotonic in-process counter is unique both across concurrent processes and across repeated
// calls within one process, without needing a platform-specific PID call.
std::filesystem::path make_temp_path() {
    static std::atomic<std::uint64_t> counter{0};
    std::random_device rd;
    const auto unique = (static_cast<std::uint64_t>(rd()) << 32) ^
                        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
                        counter.fetch_add(1, std::memory_order_relaxed);
    return std::filesystem::temp_directory_path() / ("ac3adm_" + std::to_string(unique) + ".wav");
}

// Trims the trailing ASCII spaces libbw64's bw64::AudioId pads its
// fixed-width uid()/trackRef()/packRef() fields with (see chunks.hpp:
// "init arrays with whitespaces" - std::memset(..., ' ', ...)), matching
// BS.2088-1 §8.2's own fixed-width UID[12]/trackRef[14]/packRef[11] fields.
std::string trim_trailing_spaces(std::string field) {
    const auto last = field.find_last_not_of(' ');
    field.resize(last == std::string::npos ? 0 : last + 1);
    return field;
}

std::vector<ChnaEntry> read_chna(const bw64::Bw64Reader& reader) {
    std::vector<ChnaEntry> entries;
    const auto chna_chunk = reader.chnaChunk();
    if (!chna_chunk) {
        return entries;
    }
    for (const auto& audio_id : chna_chunk->audioIds()) {
        ChnaEntry entry;
        entry.track_index = audio_id.trackIndex();
        entry.uid = trim_trailing_spaces(audio_id.uid());
        entry.track_ref = trim_trailing_spaces(audio_id.trackRef());
        entry.pack_ref = trim_trailing_spaces(audio_id.packRef());
        entries.push_back(std::move(entry));
    }
    return entries;
}

PcmAudio read_pcm(bw64::Bw64Reader& reader) {
    PcmAudio audio;
    audio.sample_rate = reader.sampleRate();
    audio.bits_per_sample = reader.bitDepth();
    const auto channel_count = reader.channels();
    if (channel_count == 0) {
        return audio;
    }
    const auto frame_count = reader.numberOfFrames();
    std::vector<float> interleaved(static_cast<std::size_t>(frame_count) * channel_count);
    reader.seek(0);
    reader.read(interleaved.data(), frame_count);

    audio.channels.assign(channel_count, std::vector<float>(frame_count));
    for (std::uint64_t frame = 0; frame < frame_count; ++frame) {
        for (std::uint16_t channel = 0; channel < channel_count; ++channel) {
            audio.channels[channel][frame] =
                interleaved[static_cast<std::size_t>(frame) * channel_count + channel];
        }
    }
    return audio;
}

std::expected<AdmModel, AdmError> read_adm_model(const bw64::Bw64Reader& reader) {
    const auto axml_chunk = reader.axmlChunk();
    // BS.2088-1 §9 rule 2: ADM metadata is optional - a file with no <axml>
    // chunk at all is still a valid BW64 file, just one with an empty
    // AdmModel (see ac3adm/model.hpp's AdmDocument comment).
    if (!axml_chunk) {
        return AdmModel{};
    }

    std::shared_ptr<adm::Document> document;
    try {
        // bw64::AxmlChunk has no data()/text() accessor of its own (its raw bytes are
        // private) - write() to a stream is the only public way to get the XML content
        // back out, so that is used here to build the istringstream adm::parseXml wants.
        std::ostringstream xml_out;
        axml_chunk->write(xml_out);
        std::istringstream xml_stream(xml_out.str());
        // recursive_node_search: without it, libadm's default parser only accepts
        // <audioFormatExtended> wrapped in the full EBUCore <ebuCoreMain><coreMetadata>
        // <format> structure (BS.2076-2 Annex 2's own worked examples use exactly that
        // wrapper) and throws XmlParsingError("audioFormatExtended node not found") on
        // anything else - confirmed by hitting this directly. Real-world ADM BWF masters
        // are not all produced by tools that add that wrapper; some embed a bare
        // <audioFormatExtended> root instead. Both are the same ADM content once found,
        // so accepting either here (rather than rejecting the bare form) is the more
        // robust choice for a production ingest reader.
        document = adm::parseXml(xml_stream, adm::xml::ParserOptions::recursive_node_search);
    } catch (const adm::error::AdmException&) {
        // Covers every adm::error:: exception libadm defines - AdmException is the common
        // base every one of them derives from (duplicate IDs, an unresolved reference, an
        // invalid enumerated value, the audioFormatExtended root not found, ...). Confirmed
        // reachable in practice for e.g. a duplicate element ID
        // (adm::error::XmlParsingDuplicateId); this is what AdmError::kMalformedAdm means.
        return std::unexpected(AdmError::kMalformedAdm);
    } catch (const std::exception&) {
        // Anything else - genuinely malformed XML (an unterminated tag, say) that never
        // reaches one of libadm's own adm::error:: types, since the lower-level XML
        // tokenizer it uses internally is a private/vendored dependency of libadm's own
        // (never exposed through any public libadm header, so this module cannot catch its
        // exact exception type without reaching past libadm's own public API boundary) -
        // BUT ALSO, confirmed directly rather than assumed, a well-formed document simply
        // missing a mandatory ADM attribute or element: libadm's own mandatory-attribute
        // check (xml_parser_helper.hpp's parseAttribute()) throws a plain, untyped
        // std::runtime_error too, indistinguishable by C++ exception type from a genuine
        // XML syntax error. AdmError::kMalformedXml covers this whole bucket - see its own
        // doc comment in ac3adm.hpp for the same caveat.
        return std::unexpected(AdmError::kMalformedXml);
    }

    return detail::build_adm_model(document);
}

std::expected<AdmDocument, AdmError> parse_bw64_path(const std::string& path) {
    std::unique_ptr<bw64::Bw64Reader> reader;
    try {
        reader = bw64::readFile(path);
    } catch (const std::exception&) {
        // libbw64 reports both "could not open" and "malformed container"
        // through the same std::runtime_error hierarchy (reader.hpp), with
        // no distinguishing exception type - kCannotOpen covers the whole
        // family here since a caller's next move (check the path) is the
        // same either way, and libbw64 does not label a chunk it dislikes
        // clearly enough to justify inventing a false-precision mapping to
        // kMissingFmt/kMissingData/kNotRiff from the exception text alone.
        return std::unexpected(AdmError::kCannotOpen);
    }

    AdmDocument document;
    try {
        document.chna = read_chna(*reader);
        document.audio = read_pcm(*reader);
    } catch (const std::exception&) {
        return std::unexpected(AdmError::kOther);
    }

    auto model = read_adm_model(*reader);
    if (!model) {
        return std::unexpected(model.error());
    }
    document.model = std::move(*model);
    return document;
}

}  // namespace

std::expected<AdmDocument, AdmError> parse_bw64(const std::string& path) {
    return parse_bw64_path(path);
}

std::expected<AdmDocument, AdmError> parse_bw64(std::istream& in) {
    if (!in) {
        return std::unexpected(AdmError::kCannotOpen);
    }
    // See ac3adm.hpp's own comment on this overload: libbw64 opens a file
    // by path internally, so an in-memory/stream source has to be spooled
    // to a real temporary file first.
    const auto temp_path = make_temp_path();
    {
        std::ofstream out(temp_path, std::ios::binary);
        if (!out) {
            return std::unexpected(AdmError::kCannotOpen);
        }
        out << in.rdbuf();
        if (!out) {
            return std::unexpected(AdmError::kCannotOpen);
        }
    }

    auto result = parse_bw64_path(temp_path.string());
    std::error_code remove_error;
    std::filesystem::remove(temp_path, remove_error);  // best-effort cleanup
    return result;
}

}  // namespace ac3adm
