#pragma once

#include <cstdint>
#include <expected>
#include <iosfwd>
#include <string>
#include <string_view>

#include "ac3adm/export.hpp"
#include "ac3adm/model.hpp"

// Top-level entry point for ac3adm::ac3adm: parses a BW64/RF64 (or plain,
// sub-4 GB RIFF/WAVE carrying the same chunks) file into an AdmDocument -
// the ADM object graph plus the <chna> join table plus the decoded PCM.
//
// Roadmap item B1 phase 1 of 3 (see ROADMAP.md's "ADM BWF reader feeding the
// JOC encoder" entry): this module knows nothing about AC-3, E-AC-3 or the
// JOC/Atmos object layer - it is a container + XML metadata reader only, the
// same "codec-blind" shape as matroska::matroska, mp4::mp4 and
// mpegts::mpegts. Mapping the parsed graph onto ac3::oba::AtmosEncoder is a
// separate, later task (phase 2); a worked end-to-end example is phase 3.
//
// Implemented on top of two vendored third-party libraries rather than a
// hand-rolled parser (see src/ac3adm/CMakeLists.txt for how they're
// fetched): libbw64 (github.com/ebu/libbw64, Apache-2.0, header-only) for
// the container layer, and libadm (github.com/ebu/libadm, Apache-2.0) for
// the ADM XML object model. Both are maintained by the same BBC/IRT team
// that authored the underlying ITU-R Recommendations themselves. Neither
// library's own types appear in this header or model.hpp - see model.hpp's
// header comment for why (the namespace collision libadm's own `adm::`
// would otherwise cause is the main reason).
//
// Primary sources these two libraries themselves implement, and that this
// module's own translation code (src/ac3adm/src/adm_model.cpp) cites where
// it makes a choice beyond "trust the library"):
//   - Recommendation ITU-R BS.2088-1 (10/2019), Annex 1: the BW64 container
//     - <ds64>, <fmt>, <data>, <chna>, <axml> chunk layouts.
//   - Recommendation ITU-R BS.2076-2 (10/2019), Annex 1: the Audio
//     Definition Model XML schema (audioProgramme/audioContent/audioObject/
//     audioPackFormat/audioChannelFormat/audioBlockFormat/audioStreamFormat/
//     audioTrackFormat/audioTrackUID) and its ID/coordinate/time-format
//     conventions.

namespace ac3adm {

enum class AdmError : std::uint8_t {
    kCannotOpen,        // path could not be opened for reading
    kNotRiff,            // not a RIFF/RF64/BW64 WAVE file
    kMissingFmt,         // no <fmt > chunk found
    kMissingData,        // no <data> chunk found
    kUnsupportedFormat,  // <fmt > names a format libbw64 cannot decode (e.g. IEEE-float/formatTag
                         // 3 - see model.hpp's PcmAudio comment). Reserved, like kNotRiff/
                         // kMissingFmt/kMissingData: libbw64 itself rejects this at open time
                         // through the same untyped exception family those three share, so it
                         // currently surfaces as kCannotOpen instead - see adm.cpp's own comment
                         // on parse_bw64_path for why inventing a false-precision mapping from
                         // the exception text alone isn't done.
    kMalformedXml,       // <axml> content failed to parse: genuinely malformed XML (an
                         // unterminated tag, say), OR well-formed XML missing a mandatory ADM
                         // attribute/element - libadm's own parser reports both through the same
                         // plain, untyped std::runtime_error (confirmed directly: a missing
                         // audioObjectID surfaces exactly this way, not as one of libadm's own
                         // typed exceptions), so this module cannot reliably tell them apart
                         // and does not claim to.
    kMalformedAdm,       // libadm rejected the document with one of its own typed
                         // adm::error::AdmException diagnostics - duplicate IDs, an unresolved
                         // reference, an invalid enumerated value, the audioFormatExtended root
                         // not found, ...
    kOther,              // any other failure surfaced by libbw64/libadm; see the exception message
                         // this can't carry - kept broad deliberately since neither library's
                         // own exception hierarchy is exposed through this API (see this
                         // header's own top comment on why not).
};

[[nodiscard]] AC3ADM_EXPORT std::string_view describe(AdmError error);

// Parses a whole BW64/RF64/RIFF file from `path`.
[[nodiscard]] AC3ADM_EXPORT std::expected<AdmDocument, AdmError> parse_bw64(const std::string& path);

// Same parse, from an already-open stream - e.g. an in-memory buffer via
// std::istringstream for testing without touching a disk. Implemented by
// spooling the stream to a temporary file and delegating to the path
// overload: libbw64's own reader opens a file by path internally (it has no
// istream constructor), so there is no way to hand it an in-memory buffer
// directly.
[[nodiscard]] AC3ADM_EXPORT std::expected<AdmDocument, AdmError> parse_bw64(std::istream& in);

}  // namespace ac3adm
