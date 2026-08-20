#pragma once

// Shared FFmpeg-oracle plumbing for ac3forge's differential fuzzing harnesses
// (fuzz_differential_ac3_decode.cpp, fuzz_differential_eac3_decode.cpp - see
// each one's own module comment for what it drives). Both harnesses decode
// the SAME mutated bytes twice: once with this project's own decoder,
// in-process, the same way fuzz_ac3_decode.cpp/fuzz_eac3_decode.cpp already
// do it, and once with FFmpeg, out-of-process, using the exact "strict
// decode" invocation this repo already treats as its external oracle
// everywhere else - tools/ci/quality_race.py's decode_scores,
// tools/checks/verify_gold_reference.sh's ffmpeg_strict_decode, and
// tools/ci/run_codec_matrix.sh's run_ffmpeg_check all use
// `-xerror -err_detect crccheck+bitstream+buffer+explode`, the flag
// combination CONTRIBUTING.md's "Oracles" section explains is required
// (`-err_detect` alone only controls what FFmpeg treats as an error
// *internally*; `-xerror` is what turns a detected error into a failing
// process). Reused verbatim here rather than re-derived.
//
// --- What counts as a reportable divergence -------------------------------
//
// FFmpeg's own error-concealment/tolerance on a mutated (i.e. potentially
// malformed) frame can legitimately differ from this project's spec-strict
// decode - a PCM mismatch on a frame either side has already flagged as
// wrong proves nothing about which one is right. So a divergence is only
// reported when BOTH decoders accepted the ENTIRE input and produced real,
// comparably-shaped audio:
//
//   1. This project's own decoder must accept every frame/access unit of
//      the input with no DecodeError, all under one unchanging acmod/
//      sample rate (see the harness .cpp files - a format change mid-stream
//      is not a shape FFmpeg's own raw PCM output could line up against
//      either, so those inputs are never even offered to FFmpeg).
//   2. FFmpeg's strict decode (see ffmpeg_strict_decode below) must exit 0
//      with a real, non-empty WAV - a non-zero exit or empty output means
//      FFmpeg declined the input, which run_differential treats as "no
//      oracle for this one", the same stance run_codec_matrix.sh already
//      takes for the tool combinations FFmpeg has no reading of at all
//      (enhanced coupling, transient pre-noise processing, a second
//      dependent substream/7.1.4 - see docs/verification.md's "Where the
//      oracles don't reach").
//   3. The two decodes must agree on sample rate and channel count, and
//      overlap by at least half of the shorter one after lag alignment -
//      anything else is a shape FFmpeg's raw PCM couldn't be compared
//      against meaningfully, not evidence of anything.
//   4. A channel with essentially no energy (below -120 dBFS) is skipped
//      rather than compared - the same "ignore near-silent frames"
//      reasoning tools/ci/quality_race.py's spectral_scores already applies,
//      because a fraction-of-an-LSB rounding difference between two
//      independent implementations can swing a near-zero ratio by tens of
//      dB without either side having done anything wrong.
//
// Under those four gates, the floor below (kMinAgreementDb) was calibrated
// against real measurements, not guessed: docs/verification.md's own
// headline numbers (float32-precision parity for the plain coding path,
// 98+ dB for coupling/spectral extension, 62-89 dB for AHT) all come from
// tools/ci/quality_race.py's broadband synthetic material, and an early
// version of this harness reused tools/checks/verify_gold_reference.sh's
// CPLBNDSTRCE0_MIN_SNR_DB=15 precedent on that basis - then
// fuzz/measure-agreement.sh (AC3FORGE_DIFF_MEASURE_ONLY=1, see
// run_differential below) run once over every file in
// fuzz/seeds/fuzz_ac3_decode/ and fuzz/seeds/fuzz_eac3_decode/ - real,
// already-shipping, unmutated content this project has never doubted - found
// two seeds well under that floor: fuzz_ac3_decode/ac3-orbit.ac3 (a panning
// source, worst channel 12.74 dB) and the plain single-tone
// ac3-sine-mono.ac3/ac3-sine-stereo.ac3/eac3-sine-mono.ec3/eac3-sine-stereo.ec3
// seeds (~22-24 dB), independently reproduced with tools/checks/compare_wav.py
// against the same two decoded WAVs to rule out a bug in this harness's own
// alignment/comparison code rather than a real property of the two decodes.
//
// The likely mechanism, consistent with both outliers: a narrow-band or
// fast-moving source leaves most MDCT coefficients per block with bap 0 (no
// mantissa transmitted), and A/52 leaves what a decoder reconstructs there
// implementation-defined ("any reasonably random sequence" per the dither
// spec) - this project's own FrameDecoder always reconstructs zero there for
// deterministic parity (see decoder.hpp's own comment on DecodedFrame),
// while FFmpeg's decoder is understood to synthesize real dither noise. A
// broadband signal leaves few bap-0 coefficients for this to matter on
// (hence the 80-130 dB seen everywhere else in the same sweep); a sparse
// spectrum leaves many. Both decoders are reading the spec correctly here -
// this is exactly the "legitimate disagreement" this header's own module
// comment warns about, just found in an unexpected corner (bap-0 dither
// policy) rather than the ecpl/tpn/spx/aht ones anticipated going in.
//
// kMinAgreementDb sits at 6 dB: comfortably below the worst measured
// legitimate case (12.74 dB, ~7 dB of margin) while still failing hard on
// the failure modes this harness exists to catch - a genuine decode bug
// (verified by deliberately introducing one - see this project's "prove the
// test can fail" rule) collapses agreement into low single digits or
// negative, not merely below double digits. Re-run fuzz/measure-agreement.sh
// after adding any new seed content, the same way this number was derived -
// it is a measurement, not a guess, and a new corner of legitimate
// disagreement would need it revisited again.

#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <vector>

#include "ac3/io/wav.hpp"

extern char** environ;

namespace ac3forge::fuzzdiff {

// Below this, two decoders' agreement on the SAME bitstream has stopped
// looking like "two spec-correct implementations reconstructing a
// generative tool differently" and started looking like "one of them got it
// wrong" - see this header's own module comment for the full reasoning,
// precedent and empirical calibration.
inline constexpr double kMinAgreementDb = 6.0;

// /dev/shm when the container provides it, /tmp otherwise - the same
// scratch-file convention fuzz_wav_read.cpp already uses, for the same
// reason (this harness's mutated input has no in-memory FFmpeg entry point
// to call instead of a real file path).
inline const char* scratch_dir() {
    static const char* const dir = (::access("/dev/shm", W_OK) == 0) ? "/dev/shm" : "/tmp";
    return dir;
}

// A fresh temp path ending in `suffix` (".ac3"/".ec3"/".wav") - FFmpeg's own
// format probe wants a real extension, matching every other FFmpeg call
// site in this repo (tools/ci/quality_race.py, tools/ci/run_codec_matrix.sh,
// tools/checks/verify_gold_reference.sh all hand it a real path rather than
// piping stdin). Empty string on failure.
inline std::string make_scratch_path(const char* suffix) {
    std::string tmpl = std::string(scratch_dir()) + "/ac3forge-diff-XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    const int fd = ::mkstemp(buf.data());
    if (fd < 0) {
        return {};
    }
    ::close(fd);
    const std::string base(buf.data());
    const std::string final_path = base + suffix;
    if (::rename(base.c_str(), final_path.c_str()) != 0) {
        ::unlink(base.c_str());
        return {};
    }
    return final_path;
}

inline bool write_file(const std::string& path, std::span<const std::byte> bytes) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        return false;
    }
    bool ok = true;
    if (!bytes.empty() && std::fwrite(bytes.data(), 1, bytes.size(), f) != bytes.size()) {
        ok = false;
    }
    std::fclose(f);
    return ok;
}

// FFmpeg's own strict decode of `coded_path` into a float32 WAV at
// `wav_out_path` (see this header's own module comment for the exact flags
// and why). posix_spawn rather than fork+exec: this harness's whole process
// is ASan/UBSan-instrumented, and a plain fork() of that is needless extra
// weight this doesn't need to pay per fuzzer iteration.
//
// Returns false on ANY sign FFmpeg declined the input - not found on PATH,
// non-zero exit, or a missing/empty output file - which the caller treats
// as "no oracle for this one", not a crash. Silences FFmpeg's own stdout/
// stderr: a fuzzer running thousands of iterations must not flood the
// console over an input this function's own return value already reports
// on.
inline bool ffmpeg_strict_decode(const std::string& coded_path, const std::string& wav_out_path) {
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    // execve-family argv entries are char*, not const char* - copy the two
    // paths so there is real, writable (if unwritten-to) storage behind
    // them rather than casting away const on a std::string's own buffer.
    std::vector<char> in_arg(coded_path.begin(), coded_path.end());
    in_arg.push_back('\0');
    std::vector<char> out_arg(wav_out_path.begin(), wav_out_path.end());
    out_arg.push_back('\0');

    char arg_prog[] = "ffmpeg";
    char arg_v[] = "-v";
    char arg_error[] = "error";
    char arg_y[] = "-y";
    char arg_xerror[] = "-xerror";
    char arg_err_detect[] = "-err_detect";
    char arg_err_detect_val[] = "crccheck+bitstream+buffer+explode";
    char arg_i[] = "-i";
    char arg_ca[] = "-c:a";
    char arg_pcm[] = "pcm_f32le";

    char* argv[] = {arg_prog,        arg_v,       arg_error,   arg_y,          arg_xerror,
                     arg_err_detect, arg_err_detect_val, arg_i, in_arg.data(), arg_ca,
                     arg_pcm,        out_arg.data(),      nullptr};

    ::pid_t pid = 0;
    const int spawn_rc = ::posix_spawnp(&pid, "ffmpeg", &actions, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (spawn_rc != 0) {
        return false;  // ffmpeg not on PATH, or posix_spawn itself failed
    }
    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) {
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return false;
    }
    struct stat st{};
    return ::stat(wav_out_path.c_str(), &st) == 0 && st.st_size > 0;
}

// How many samples `b` leads (positive) or lags (negative) `a` by, found by
// cross-correlating a short prefix - the same reason
// tools/checks/compare_wav.py's best_lag exists: decoder priming/lookahead can
// shift two independently-implemented decoders' output by a handful of
// samples even for the identical bitstream. +-512 matches that script's own
// max-lag default; the probe itself is capped small because this harness's
// inputs (mutated single-to-few-frame streams) never need a long one.
inline int best_lag(const std::vector<float>& a, const std::vector<float>& b) {
    constexpr int kMaxLag = 512;
    const std::size_t probe_len = std::min<std::size_t>({a.size(), b.size(), 8192});
    if (probe_len == 0) {
        return 0;
    }
    int best_lag_value = 0;
    double best_score = -1.0;
    for (int lag = -kMaxLag; lag <= kMaxLag; ++lag) {
        double score = 0.0;
        std::size_t overlap = 0;
        for (std::size_t i = 0; i < probe_len; ++i) {
            const long bi = static_cast<long>(i) + lag;
            if (bi < 0 || static_cast<std::size_t>(bi) >= b.size()) {
                continue;
            }
            score += static_cast<double>(a[i]) * static_cast<double>(b[static_cast<std::size_t>(bi)]);
            ++overlap;
        }
        if (overlap < probe_len / 4) {
            continue;  // too little overlap at this lag to trust the score
        }
        if (score > best_score) {
            best_score = score;
            best_lag_value = lag;
        }
    }
    return best_lag_value;
}

struct CompareResult {
    bool comparable = false;
    double worst_channel_snr_db = 0.0;
};

// Aligns `ours` against `theirs` (best_lag on a mono downmix), then reports
// the worst-channel SNR across every channel that carries real signal - see
// this header's own module comment for the near-silence and low-overlap
// skip rules. `comparable` is false whenever the two results are not in a
// shape worth comparing at all (channel-count mismatch, or too little
// overlap once aligned) - the caller must not treat that as agreement, only
// as "nothing to say here".
inline CompareResult compare_pcm(const std::vector<std::vector<float>>& ours,
                                  const std::vector<std::vector<float>>& theirs) {
    CompareResult result;
    if (ours.empty() || theirs.empty() || ours.size() != theirs.size()) {
        return result;
    }

    std::vector<float> mono_ours(ours.front().size(), 0.0F);
    for (const auto& ch : ours) {
        for (std::size_t i = 0; i < ch.size() && i < mono_ours.size(); ++i) {
            mono_ours[i] += ch[i];
        }
    }
    std::vector<float> mono_theirs(theirs.front().size(), 0.0F);
    for (const auto& ch : theirs) {
        for (std::size_t i = 0; i < ch.size() && i < mono_theirs.size(); ++i) {
            mono_theirs[i] += ch[i];
        }
    }
    const int lag = best_lag(mono_ours, mono_theirs);

    // -120 dBFS, squared (this compares sums of squared magnitude, not
    // amplitude) - see the module comment's near-silence rule.
    constexpr double kSilenceFloorSq = 1e-12;

    double worst = 1.0e300;
    bool any_channel_scored = false;
    for (std::size_t ch = 0; ch < ours.size(); ++ch) {
        const auto& a = ours[ch];
        const auto& b = theirs[ch];
        double signal = 0.0;
        double noise = 0.0;
        std::size_t overlap = 0;
        for (std::size_t i = 0; i < a.size(); ++i) {
            const long bi = static_cast<long>(i) + lag;
            if (bi < 0 || static_cast<std::size_t>(bi) >= b.size()) {
                continue;
            }
            const double sv = static_cast<double>(a[i]);
            const double d = sv - static_cast<double>(b[static_cast<std::size_t>(bi)]);
            signal += sv * sv;
            noise += d * d;
            ++overlap;
        }
        if (overlap < std::min(a.size(), b.size()) / 2) {
            return CompareResult{};  // not enough overlap to trust any verdict at all
        }
        if (signal < kSilenceFloorSq * static_cast<double>(overlap)) {
            continue;  // near-digital-silence on this channel - skip, don't fail it
        }
        const double snr_db = (noise <= 1e-30) ? 200.0 : 10.0 * std::log10(signal / noise);
        worst = std::min(worst, snr_db);
        any_channel_scored = true;
    }
    if (!any_channel_scored) {
        return result;  // every channel was silent - nothing meaningful to compare
    }
    result.comparable = true;
    result.worst_channel_snr_db = worst;
    return result;
}

// Prints a diagnostic and aborts - a divergence has to become an actual
// process crash for libFuzzer to catch, minimize and save it the same way
// it already does for a real memory-safety bug (see fuzz/README.md's "when
// a fuzzer finds something"). Deliberately std::abort(), not assert(): the
// fuzz build is RelWithDebInfo, which defines NDEBUG by default (see the
// clang-tidy assert/NDEBUG blindspot this project has already been bitten
// by once), so an assert() here would silently compile out.
[[noreturn]] inline void report_divergence(const char* codec_label, double worst_channel_snr_db) {
    std::fprintf(stderr,
                 "ac3forge differential fuzzer: %s decode diverges from FFmpeg's own decode of "
                 "the SAME bitstream (worst-channel agreement %.2f dB, floor %.2f dB) - both "
                 "decoders accepted this input as valid. See fuzz/README.md's \"when a fuzzer "
                 "finds something\" section.\n",
                 codec_label, worst_channel_snr_db, kMinAgreementDb);
    std::fflush(stderr);
    std::abort();
}

// The whole pipeline for one fuzzer input: write `coded_bytes` to a scratch
// file, have FFmpeg strict-decode it, write THIS project's own decode
// (`pcm`/`sample_rate`/`channel_order` - same shape ac3::io::write_wav_f32
// itself takes, `channel_order` empty meaning identity) to a second scratch
// WAV, read both back and compare. Aborts (see report_divergence) exactly
// when the comparison is eligible AND disagrees by more than
// kMinAgreementDb; returns quietly in every other case, per this header's
// own module comment.
//
// Two environment variables change this function's behaviour for a human
// investigating a specific input rather than for a fuzzing run - neither is
// ever set by fuzz.yml or a normal `fuzz/run.sh` invocation:
//   AC3FORGE_DIFF_MEASURE_ONLY  prints every comparable result and never
//                               aborts - see fuzz/measure-agreement.sh.
//   AC3FORGE_DIFF_DEBUG         keeps the three scratch files instead of
//                               deleting them and prints their paths, so a
//                               specific divergence (or a specific seed
//                               during calibration) can be inspected by
//                               hand, e.g. with tools/checks/compare_wav.py.
inline void run_differential(const char* codec_label, std::span<const std::byte> coded_bytes,
                              const char* coded_ext, const std::vector<std::vector<float>>& pcm,
                              std::uint32_t sample_rate, std::span<const std::size_t> channel_order) {
    const std::string coded_path = make_scratch_path(coded_ext);
    if (coded_path.empty()) {
        return;
    }
    const std::string ffmpeg_wav_path = make_scratch_path(".wav");
    const std::string ours_wav_path = make_scratch_path(".wav");

    // AC3FORGE_DIFF_MEASURE_ONLY: prints every comparable result without
    // ever aborting - a calibration aid for picking kMinAgreementDb from
    // real measurements across a whole corpus rather than guessing (see
    // this header's own module comment), not something either the CI job or
    // a normal local run ever sets.
    const bool measure_only = ::getenv("AC3FORGE_DIFF_MEASURE_ONLY") != nullptr;

    bool divergence = false;
    double snr = 0.0;
    if (!ffmpeg_wav_path.empty() && !ours_wav_path.empty() &&
        write_file(coded_path, coded_bytes) &&
        ffmpeg_strict_decode(coded_path, ffmpeg_wav_path) &&
        ac3::io::write_wav_f32(ours_wav_path, std::span<const std::vector<float>>(pcm),
                                sample_rate, channel_order)) {
        const auto ffmpeg_wav = ac3::io::read_wav(ffmpeg_wav_path);
        const auto ours_wav = ac3::io::read_wav(ours_wav_path);
        if (ffmpeg_wav && ours_wav && ffmpeg_wav->sample_rate == ours_wav->sample_rate) {
            const auto result = compare_pcm(ours_wav->channels, ffmpeg_wav->channels);
            if (measure_only) {
                std::fprintf(stderr, "AC3FORGE_DIFF_MEASURE_ONLY: %s comparable=%d worst_db=%.2f\n",
                             codec_label, static_cast<int>(result.comparable),
                             result.worst_channel_snr_db);
            }
            if (result.comparable && result.worst_channel_snr_db < kMinAgreementDb) {
                divergence = true;
                snr = result.worst_channel_snr_db;
            }
        }
    }
    if (measure_only) {
        divergence = false;  // never abort in calibration mode
    }

    if (::getenv("AC3FORGE_DIFF_DEBUG") == nullptr) {
        ::unlink(coded_path.c_str());
        if (!ffmpeg_wav_path.empty()) {
            ::unlink(ffmpeg_wav_path.c_str());
        }
        if (!ours_wav_path.empty()) {
            ::unlink(ours_wav_path.c_str());
        }
    } else {
        std::fprintf(stderr, "AC3FORGE_DIFF_DEBUG: coded=%s ffmpeg_wav=%s ours_wav=%s\n",
                     coded_path.c_str(), ffmpeg_wav_path.c_str(), ours_wav_path.c_str());
    }

    if (divergence) {
        report_divergence(codec_label, snr);
    }
}

}  // namespace ac3forge::fuzzdiff
