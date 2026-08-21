#pragma once

#include <cstdint>
#include <cstdio>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ac3/analysis/levels.hpp"
#include "ac3/encoder/plan.hpp"
#include "ac3/io/wav.hpp"
#include "ac3/meta/loudness.hpp"
#include "matroska/matroska.hpp"

// The CLI-wide support layer: option/metadata parsing, path/stdio conventions, frame and WAV I/O,
// and level reporting shared by nearly every command in main.cpp's kCommands table. Split out of
// main.cpp (which used to define all of this in its own anonymous namespace) as the first step of
// the repo-structure review's H4 monolith split - see that review for why, and main.cpp's own
// command-table comment for the design these helpers serve.
//
// Everything here has external linkage (namespace ac3cli, not main.cpp's old anonymous namespace)
// because it is now called from a different translation unit. A few helpers that are genuinely
// private to one function's own implementation (parse_double, to_bytes, write_wav_f32_arg) stay
// out of this header entirely and live in an anonymous namespace inside support.cpp instead,
// preserving the original "internal unless something else needs it" default.
//
// Everything about layouts, coding tools and metadata itself lives in ac3::plan, so the GUI
// cannot mean something different by "514" or by "all" than this does. What is here is argument
// shape, validation and printing - Options carries plan::Metadata verbatim rather than a second,
// CLI-specific copy of the same fields.
namespace ac3cli {

std::uint32_t parse_u32_or(std::string_view text, std::uint32_t fallback);

// --- metadata options -------------------------------------------------------
// Bare words and key=value tokens, appended after the positional arguments in
// any order, the same way 'couple' already works. Everything defaults off, so
// a command line that says nothing about metadata produces exactly the stream
// it produced before this layer existed.

void print_meta_usage();

// Everything a command accepts after its positional arguments, in any order.
// The metadata group is ac3::plan::Metadata verbatim; drc_scale is decode-
// side local, because nothing an encoder is configured with corresponds to
// it; sources/map_spec describe routing rather than metadata, but share this
// same trailing-options surface (parse_options) the way dialnorm2= already
// shares it despite being layout-1+1-specific - a command that has no use
// for a field simply never sets it.
struct Options {
    ac3::plan::Metadata p{};
    // Decoder side, for 'decode'.
    double drc_scale = 0.0;
    // Each src= occurrence, in order given - additional input sources beyond
    // the primary positional argument. encode/eac3-encode only; empty unless
    // multi-source input is in play.
    std::vector<std::string> sources;
    // The raw map= text, if given - parsed into a plan::Assignment once the
    // sources are loaded and their channel counts are known, which
    // parse_options itself cannot do (it only sees command-line text, not
    // opened files).
    std::optional<std::string> map_spec;
    // Each offset= occurrence: (sourceIndex, seconds) - leading silence ahead
    // of that source's own audio, in the same 0-based numbering src=
    // establishes (0 = the primary positional argument, 1..N = each src= in
    // order). encode/eac3-encode only, including the classic single-file
    // path, where source 0 is the only source there is. A given sourceIndex
    // may appear more than once; the last occurrence wins (see
    // offset_samples_for).
    std::vector<std::pair<std::size_t, double>> offsets;
    // Atmos object signing (atmos/atmos-path/atmos-encode). Off unless the
    // operator both asks (sign-objects) and provides a key - either
    // signing-key=<path> here, or the AC3FORGE_SIGNING_KEY[_FILE] env vars
    // load_signing_key() falls back to. The key is never stored by this tool;
    // see docs/concepts/object-signing.md.
    bool sign_objects = false;
    std::optional<std::string> signing_key;
    // 'decode'/'monitor' only: check each frame's EMDF object container
    // against signing-key= (same option sign-objects uses - a decode never
    // signs, so there is no ambiguity in sharing it) instead of just playing
    // it. Off by default: a signed-but-unchecked stream decodes exactly like
    // an unsigned one unless the operator opts in here - see
    // docs/concepts/object-signing.md.
    bool verify_objects = false;
    // 'live' only: a second ("slave") capture device index, same numbering
    // ac3::audio::enumerate_devices()/'devices' uses and the capture_device
    // positional already reads. Unset means the classic single-device
    // session, unchanged from before this option existed.
    std::optional<int> capture2 = std::nullopt;
    // 'record'/'live' only: write straight to Matroska instead of the bare
    // elementary stream they write by default - the same shape of choice the
    // GUI's own Container combo offers (EncoderController::containerIndex ==
    // kContainerMatroska), see write_frames_or_mux. Off by default, matching
    // every bare-token/off-by-default field here: a plain invocation writes
    // exactly the .ac3/.ec3 it always has.
    bool matroska_container = false;
    // Off by default, matching every bare token here - keep whatever frames
    // a failed encode already produced, written beside the intended output
    // as <name>.partial.<ext> instead of discarded outright. The same
    // "named and kept, never silently discarded" behaviour the GUI's own
    // keepPartialOutput preference gives EncoderController's file encodes
    // (see gui/encoder_controller.cpp's partial_output_path), offered here
    // per invocation rather than as a standing preference - see
    // write_partial_output.
    bool keep_partial = false;
    // The §7.9.4 fast forward MDCT (plan::Tools::fast_mdct), on by default
    // like the library configs it feeds; fast-mdct=off forces the direct
    // §8.2.3.2 reference form wherever this command encodes (encode/sine and
    // the atmos/record/live session builders), the same key=off shape
    // surmixlev=/lfemix= already use. E-AC-3's own tools= string reaches the
    // same field with its own tokens ("nofastmdct" to force direct, matching
    // "noatten"; the old opt-in "fastmdct" parses as a no-op) - AC-3 has no
    // tools= string to extend, so this option is its equivalent, the same
    // relationship 'couple' has to cpl/cpl:N. The bare word 'fast-mdct'
    // (the opt-in spelling from when this defaulted off) stays accepted and
    // now names what already happens.
    bool fast_mdct = true;
    // 'qc' only: which delivery gate(s) to check the measurement against -
    // one of ac3::meta::kQcPresetNames, or "all" to check every preset.
    // Unset (measure-only, no gate) is the default - a plain
    // 'ac3cli qc <file>' just reports the numbers, no pass/fail verdict.
    std::optional<std::string> qc_preset;
};

// Returns false and prints the offending token on anything unrecognised: a
// silently ignored metadata flag looks exactly like metadata that did not work.
bool parse_options(std::span<char*> tokens, Options& out);

// Reads a loudness measurement someone else already pushed every sample
// into, reports it the same way every dialnorm=auto path does, and returns
// the dialnorm it implies. Factored out of measured_dialnorm/
// measured_dialnorm_channel below so a measurement built incrementally
// across many frames (the src=/map= routed-programme pre-pass) reports
// itself identically to one built from a single whole-buffer push - same
// text, same rounding, one place either could go wrong. `programme` is the
// println's leading label ("Ch1"/"Ch2"), empty for a whole-programme
// measurement that is not about one dual-mono channel; `field` is the
// bitstream field this measurement feeds ("dialnorm"/"dialnorm2"). `out`
// defaults to stdout for callers with no "-" output stream to protect (the
// standalone loudness command); every dialnorm=auto/dialnorm2=auto encode
// path passes status_stream(out) instead, the same convention
// print_channel_summary and print_routing use.
std::optional<int> finish_measurement(const ac3::meta::LoudnessMeter& meter,
                                      std::string_view programme, std::string_view field,
                                      FILE* out = stdout);

// BS.1770 integrated loudness of a whole WAV, and the dialnorm it implies.
// Never meaningful for a dual-mono (1+1) target - Ch1 and Ch2 are two
// unrelated programmes sharing one syncframe (§E1.3, no downmix between
// them), so a single BS.1770 pass across both channels would measure a
// blend of two different things rather than either programme's own level;
// callers route dual mono through measured_dialnorm_channel on each
// programme's own channel alone instead.
std::optional<int> measured_dialnorm(const ac3::io::WavData& wav, ac3::SampleRate rate,
                                     ac3::Acmod acmod, bool lfe, FILE* out = stdout);

// Same measurement, for one dual-mono programme's own channel alone - never a
// programme's worth of BS.1770 surround weighting, since a 1+1 channel is not
// part of a soundfield. `programme`/`field` are finish_measurement's own
// labels above - "Ch1"/"dialnorm" or "Ch2"/"dialnorm2", the two programmes
// sharing this one function since the measurement itself does not differ.
std::optional<int> measured_dialnorm_channel(std::span<const float> channel, ac3::SampleRate rate,
                                             std::string_view programme, std::string_view field,
                                             FILE* out = stdout);

// Dual mono's Ch1/Ch2 arrive as either one two-channel file or two mono ones;
// this settles which shape `wav` is in and merges a second file's channel in
// when there is one, so everything downstream sees a plain two-channel source
// the same way it always has - `plan::route`'s own 1+1 handling only ever
// looks at the channel count, never how many files it came from.
bool prepare_dual_mono_source(ac3::io::WavData& wav, std::string_view layout,
                              std::string_view in2_path);

// The conventional Unix "-" file argument: a lone dash means stdin for an
// input path or stdout for an output path, the same convention ffmpeg, sox
// and most other Unix tools use for pipe-based workflows (e.g.
// `ac3cli encode - - 448 couple < in.wav > out.ac3`). Checked by exact
// string match only - a path that merely starts with '-' is an ordinary
// (if oddly named) filename, not this convention.
bool is_stdio_path(std::string_view path);

// Where a command's human-readable status report goes, once out_path's own
// destination is settled: stdout as always, unless out_path IS "-" - the
// binary payload itself is going to stdout then, and a status line like
// "encoded N frames..." landing in the middle of that stream would corrupt
// whatever is reading it downstream. The same split ffmpeg and friends make
// between their progress/log output and the media they actually pipe.
FILE* status_stream(std::string_view out_path);

bool write_frames(std::string_view path, std::span<const std::vector<std::byte>> frames);

// Writes `frames` either as a bare elementary stream (write_frames above) or,
// when `matroska` is set, muxed into Matroska - the choice 'record'/'live's
// own container= token (and the GUI's Container combo) offer. `track` is
// built by the caller from what it already knows about the session (codec,
// sample rate, coded channel count) rather than scanned off the bitstream
// the way 'mkv' reads an arbitrary already-encoded file: record/live just
// finished constructing the encoder themselves, so there is nothing to
// rediscover. Kept beside write_frames rather than folded into it - most
// callers have no AudioTrack to give it, and 'mkv' itself stays separate too,
// since ITS track comes from ac3::io::scan(), not a caller-supplied one.
bool write_frames_or_mux(std::string_view path, bool matroska, const matroska::AudioTrack& track,
                         std::span<const std::vector<std::byte>> frames);

// Where a failed encode's frames land when keep-partial is given: ".partial"
// spliced in before the suffix, so "out.ec3" keeps its half-finished take as
// "out.partial.ec3" - the same naming EncoderController::partial_output_path
// gives the GUI's own keepPartialOutput preference (see gui/
// encoder_controller.cpp), so a file produced either way is named alike.
std::string partial_output_path(std::string_view path);

// One frame written `count` times, for the silence generators: they used to
// materialise `count` identical copies of a single ~2 KB frame first (~268
// MB for an hour of silence) purely to satisfy write_frames' list shape.
bool write_repeated_frame(std::string_view path, std::span<const std::byte> frame,
                          std::uint64_t count);

// Writes whatever frames a failed encode already produced to
// partial_output_path(out_path) when keep_partial asked for it and there is
// at least one - "named and kept, never silently discarded", the same rule
// the GUI's own keep-partial-output preference follows. A no-op (silently)
// when keep_partial is false or nothing was encoded yet; a write failure for
// the partial itself is reported but does not change the caller's own exit
// code, since the ORIGINAL error is still the one that matters.
void write_partial_output(std::string_view out_path, bool keep_partial,
                          std::span<const std::vector<std::byte>> frames);

// Interleaves `channels` (one vector per decoded channel, AC-3/E-AC-3 coded
// order) into WAV/Windows speaker order for playback, reading order[i] as
// which channels[] entry belongs at interleaved position i - the same
// permutation ac3::io::write_wav_f32 and plan::wav_order/wav_channel_order
// already produce for exactly this AC-3-order-vs-WAV-order reconciliation
// (see ac3/io/wav.hpp).
std::vector<float> interleave_reordered(std::span<const std::vector<float>> channels,
                                        std::span<const std::size_t> order);

std::vector<std::byte> read_all(std::string_view path);

// Wraps ac3::io::read_wav to honor the "-" stdin convention (is_stdio_path
// above): "-" reads the WAV from stdin, binary mode set first, instead of
// opening a file with that literal name.
std::expected<ac3::io::WavData, ac3::io::WavError> read_wav_arg(std::string_view path);

// Streams planar float channels into a WAV as they decode, so the decoded
// programme never sits in memory whole (it used to: ~69 MB per minute of
// 5.1). Channels arrive per SLOT and may momentarily advance unevenly -
// E-AC-3's transient-pre-noise flush appends per mapped slot - so each slot
// keeps a small carry, and whole interleaved frames go to WavStreamWriter as
// soon as every slot has them. Two deliberate fallbacks: out_path "-"
// accumulates and writes in one shot at close (stdout cannot seek, and
// WavStreamWriter patches its header), and any residue left by slots of
// unequal final length is dropped with a warning - the whole-buffer write
// this replaces indexed every channel to the first one's length, so equal
// lengths are the only case that ever actually occurred.
class PlanarWavSink {
   public:
    // `order`: entry i names the source slot that belongs at WAV position i
    // (write_wav_f32's convention); empty means identity.
    [[nodiscard]] bool open(std::string_view path, std::uint32_t sample_rate, std::size_t slots,
                            std::span<const std::size_t> order);

    [[nodiscard]] bool is_open() const { return open_; }

    [[nodiscard]] bool append(std::size_t slot, std::span<const float> samples);

    // Finalize; reports whether the write side stayed healthy. Unequal
    // residue across slots (never produced by a healthy stream) is dropped.
    [[nodiscard]] std::expected<void, ac3::io::WavError> close();

    // The decode failed part-way: close and remove whatever was written, so
    // a failed run leaves no output file - exactly like the whole-buffer
    // write it replaces, which never ran at all on failure.
    void abort();

   private:
    [[nodiscard]] bool drain();

    std::string path_;
    bool stdio_ = false;
    bool open_ = false;
    std::uint32_t sample_rate_ = 0;
    ac3::io::WavStreamWriter writer_;
    std::vector<std::vector<float>> slots_;
    std::vector<std::size_t> consumed_;
    std::vector<std::size_t> order_;
    std::vector<float> scratch_;
};

// ---------------------------------------------------------------------------
// Level reporting. Every number comes from ac3::analysis, so a level reads
// the same here as on the GUI's meters; only the drawing is local.
// ---------------------------------------------------------------------------

// A bar on the same -60..0 dBFS scale the GUI's meters use. ASCII rather than
// block glyphs: this has to stay legible in a bare console whatever code page
// it happens to be running.
std::string meter_bar(double db, int width);

// The exact figures for a finished run. Peak and RMS here are unweighted over
// the whole signal — ballistics exist to make a moving display readable, and
// would only blur a question that has a right answer.
// `out` defaults to stdout for every existing caller; the only ones that
// pass anything else are the "-" stdout-output commands (encode/eac3-encode/
// atmos-encode/decode), which redirect it to stderr so this human-readable
// report doesn't land in the middle of the binary stream those commands may
// be writing to the very same stdout - see status_stream()'s own comment.
void print_channel_summary(const ac3::analysis::LevelMeter& meter, FILE* out = stdout);

// One line, rewritten in place. A carriage return rather than ANSI cursor
// moves, so it behaves the same in a bare console as in a terminal that
// speaks escape sequences. Every field is fixed width, so the line never
// leaves fragments of a longer previous line behind.
void print_live_meter(const ac3::analysis::LevelMeter& meter, double seconds);

// Sets `plan`'s channels from `name` and writes a human-readable label for
// it into `label`, reporting a bad token against the set the codec can
// actually carry (so asking AC-3 for 7.1.4 says which of the two things is
// wrong) or false on anything neither a named layout nor a channel list
// accepts. Tried in that order: a name recognised by parse_layout wins, so a
// custom list can never shadow one of the seven presets.
bool resolve_layout(std::string_view name, ac3::plan::Codec codec, ac3::plan::Plan& plan,
                    std::string& label);

}  // namespace ac3cli
