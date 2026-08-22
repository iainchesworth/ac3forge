#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/io/wav.hpp"
#include "ac3/verify/mirror.hpp"
#include "ac3/verify/selfcheck.hpp"

// ac3::verify - the encoder/decoder mirror check. See ac3/verify/mirror.hpp
// for what it compares and why the bit offset at each block boundary is the
// part that does the heavy lifting.
//
// Two kinds of test live here. The compare() cases below plant a divergence
// in a pair of hand-built traces and check that the right block, stream and
// field come back out - they are what proves the check can FAIL, without
// needing a broken encoder to prove it with. The end-to-end cases then run
// real encodes through MirrorEncoder and require silence.

namespace {

using ac3::verify::Field;

// A trace of one frame, `blocks` blocks deep, that both sides agree on:
// `streams` streams each, every array a constant, no delta correction. A test
// then perturbs one copy.
ac3::verify::FrameTrace flat_trace(int fbw, int coded, int streams, int blocks) {
    ac3::verify::FrameTrace trace;
    trace.fbw_channels = fbw;
    trace.coded_channels = coded;
    for (int block = 0; block < blocks; ++block) {
        auto& b = trace.blocks[static_cast<std::size_t>(block)];
        b.entered = true;
        b.allocated = true;
        b.bit_offset = static_cast<std::size_t>(1000 + 500 * block);
        b.deltbaie = false;
        b.streams.assign(static_cast<std::size_t>(streams), {});
        for (auto& stream : b.streams) {
            stream.exponents.assign(64, 7);
            stream.bap.assign(64, 3);
        }
    }
    return trace;
}

}  // namespace

TEST_CASE("verify::compare passes an encoder and decoder that agree", "[verify]") {
    const auto encoder = flat_trace(2, 2, 3, ac3::kBlocksPerFrame);
    const auto decoder = encoder;
    CHECK(ac3::verify::compare(encoder, decoder, 0).empty());
}

TEST_CASE("verify::compare reports the block boundary a desync starts at", "[verify]") {
    const auto encoder = flat_trace(2, 2, 3, ac3::kBlocksPerFrame);
    auto decoder = encoder;
    // A decoder that sized one field differently arrives at block 3 short. It
    // stays wrong for every block after that, which is exactly what a real
    // desync does - and the report must still name block 3.
    for (int block = 3; block < ac3::kBlocksPerFrame; ++block) {
        decoder.blocks[static_cast<std::size_t>(block)].bit_offset -= 17;
    }

    const auto found = ac3::verify::compare(encoder, decoder, 12);
    REQUIRE_FALSE(found.empty());
    CHECK(found.front().frame == 12);
    CHECK(found.front().block == 3);
    CHECK(found.front().field == Field::kBitOffset);
    CHECK(found.front().encoder == 2500);
    CHECK(found.front().decoder == 2483);
    // Only the first divergent block is reported: blocks 4 and 5 differ too,
    // and listing them would bury the one line that names the cause.
    for (const auto& mismatch : found) {
        CHECK(mismatch.block == 3);
    }
}

TEST_CASE("verify::compare names the stream and field a delta divergence is in", "[verify]") {
    const auto encoder = flat_trace(2, 2, 3, ac3::kBlocksPerFrame);
    auto decoder = encoder;
    // The shape of the deltbaie bug this facility was built for: the encoder
    // has dropped a correction, the decoder is still holding it, and the two
    // agree on every bit written so far - block 2's offsets still match. The
    // damage lands later; the cause is here.
    auto& held = decoder.blocks[2].streams[1].delta;
    held.deltnseg = 1;
    held.deltoffst[0] = 4;
    held.deltlen[0] = 3;
    held.deltba[0] = 5;

    const auto found = ac3::verify::compare(encoder, decoder, 0);
    REQUIRE_FALSE(found.empty());
    CHECK(found.front().block == 2);
    CHECK(found.front().stream == 1);
    CHECK(found.front().field == Field::kDeltaSegmentCount);
    CHECK(found.front().encoder == 0);
    CHECK(found.front().decoder == 1);
    CHECK(ac3::verify::describe(found.front(), 2, 2) ==
          "frame 0 block 2 channel 1: deltnseg encoder=0 decoder=1");
}

TEST_CASE("verify::compare reports a bap divergence against its bin", "[verify]") {
    const auto encoder = flat_trace(3, 4, 5, ac3::kBlocksPerFrame);
    auto decoder = encoder;
    decoder.blocks[1].streams[4].bap[42] = 9;

    const auto found = ac3::verify::compare(encoder, decoder, 3);
    REQUIRE_FALSE(found.empty());
    CHECK(found.front().block == 1);
    CHECK(found.front().stream == 4);
    CHECK(found.front().index == 42);
    CHECK(found.front().field == Field::kBap);
    // Stream 4 is neither a channel nor the LFE in a 3-fbw, 4-coded frame.
    CHECK(ac3::verify::describe(found.front(), 3, 4) ==
          "frame 3 block 1 coupling: bap[42] encoder=3 decoder=9");
}

TEST_CASE("verify::compare caps how much of one desynced array it reports", "[verify]") {
    const auto encoder = flat_trace(1, 1, 1, ac3::kBlocksPerFrame);
    auto decoder = encoder;
    // Every bin of the allocation differs, which is what a real desync does to
    // one. The identity of the block and stream is the finding; the other 60
    // bins are the same finding repeated.
    for (auto& value : decoder.blocks[0].streams[0].bap) {
        value = 1;
    }

    const auto found = ac3::verify::compare(encoder, decoder, 0);
    CHECK(static_cast<int>(found.size()) == ac3::verify::kMaxPerArray);
}

TEST_CASE("verify::compare reports a decoder that stopped early", "[verify]") {
    const auto encoder = flat_trace(2, 2, 3, ac3::kBlocksPerFrame);
    auto decoder = encoder;
    // A refused frame leaves the trace ending where the decoder gave up.
    for (int block = 4; block < ac3::kBlocksPerFrame; ++block) {
        decoder.blocks[static_cast<std::size_t>(block)].entered = false;
        decoder.blocks[static_cast<std::size_t>(block)].allocated = false;
    }

    const auto found = ac3::verify::compare(encoder, decoder, 0);
    REQUIRE_FALSE(found.empty());
    CHECK(found.front().block == 4);
    CHECK(found.front().field == Field::kBlockReached);
}

TEST_CASE("verify::compare reports a block the decoder entered but never allocated", "[verify]") {
    const auto encoder = flat_trace(2, 2, 3, ac3::kBlocksPerFrame);
    auto decoder = encoder;
    // §7.10.2's guards fire between entering a block and allocating it - an
    // exponent outside 0..24, a grouped exponent above 124 - so this is the
    // shape a refusal actually leaves behind, and the LAST block with a
    // matching bit offset is the one worth looking at first.
    decoder.blocks[2].allocated = false;
    decoder.blocks[2].streams.clear();
    for (int block = 3; block < ac3::kBlocksPerFrame; ++block) {
        decoder.blocks[static_cast<std::size_t>(block)].entered = false;
        decoder.blocks[static_cast<std::size_t>(block)].allocated = false;
    }

    const auto found = ac3::verify::compare(encoder, decoder, 0);
    REQUIRE_FALSE(found.empty());
    CHECK(found.front().block == 2);
    CHECK(found.front().field == Field::kAllocationReached);
}

// --- end to end ------------------------------------------------------------

namespace {

// Runs every frame of `channels` through MirrorEncoder and returns the first
// frame that failed the check, rendered, or an empty string if all of them
// passed. Catch2's REQUIRE then carries the whole thing into the failure
// message: frame, block, channel, field, both values.
std::string mirror_encode(const ac3::EncoderConfig& config,
                          const std::vector<std::vector<float>>& channels) {
    ac3::verify::MirrorEncoder encoder{config};
    const std::size_t frames = channels.front().size() / ac3::kSamplesPerFrame;
    std::vector<std::span<const float>> views(channels.size());
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (std::size_t ch = 0; ch < channels.size(); ++ch) {
            views[ch] = std::span{channels[ch]}.subspan(frame * ac3::kSamplesPerFrame,
                                                        ac3::kSamplesPerFrame);
        }
        const auto checked = encoder.encode_frame(views);
        if (!checked) {
            return "frame " + std::to_string(frame) + ": encode failed";
        }
        if (checked->ok()) {
            continue;
        }
        std::string out = encoder.last_report();
        if (checked->decode_error) {
            // The refusal is the symptom; whatever compare() found above it is
            // the cause, and the gap between the two is the point.
            if (!out.empty()) {
                out += "\n";
            }
            out += "frame " + std::to_string(frame) + ": decoder refused the frame (" +
                   std::string{ac3::describe(*checked->decode_error)} + ")";
        }
        return out;
    }
    return {};
}

std::vector<std::vector<float>> golden_audio(const std::string& name) {
    auto wav = ac3::io::read_wav(std::string{AC3FORGE_GOLDEN_AUDIO_DIR} + "/" + name);
    REQUIRE(wav.has_value());
    return wav->channels;
}

}  // namespace

// The material and rates the deltbaie bug (bugfix/deltbaie-stale-delta-bit-
// allocation) actually reproduced on, which is why they are pinned here rather
// than left to a synthetic signal: a run's delta correction only ends part-way
// through a frame when the programme really does go quiet mid-frame, and
// reference_stereo.wav does. 64 and 96 kbit/s were the two rates that produced
// undecodable frames.
TEST_CASE("encoder and decoder agree on real stereo programme material", "[verify][golden]") {
    const auto channels = golden_audio("reference_stereo.wav");
    for (const std::uint32_t kbps : {48u, 64u, 96u, 128u, 192u, 256u}) {
        CAPTURE(kbps);
        ac3::EncoderConfig config;
        config.acmod = ac3::Acmod::k2_0;
        config.bitrate_kbps = kbps;
        const auto failure = mirror_encode(config, channels);
        INFO(failure);
        CHECK(failure.empty());
    }
}

TEST_CASE("encoder and decoder agree with coupling active", "[verify][golden]") {
    const auto channels = golden_audio("reference_stereo.wav");
    for (const std::uint32_t kbps : {96u, 128u, 192u}) {
        CAPTURE(kbps);
        ac3::EncoderConfig config;
        config.acmod = ac3::Acmod::k2_0;
        config.bitrate_kbps = kbps;
        // Coupling adds a whole extra stream to the comparison, with its own
        // exponents, its own allocation region and its own delta correction.
        config.coupling = true;
        const auto failure = mirror_encode(config, channels);
        INFO(failure);
        CHECK(failure.empty());
    }
}

TEST_CASE("encoder and decoder agree on 5.1 with an LFE", "[verify][golden]") {
    const auto wav = golden_audio("reference_51.wav");
    const auto layout = ac3::io::ac3_layout_for(wav.size());
    REQUIRE(layout.has_value());
    // WAV order is not A/52 Table 5.8's; reorder before the encoder sees it.
    std::vector<std::vector<float>> channels;
    for (const auto position : layout->wav_index) {
        channels.push_back(wav[position]);
    }
    for (const std::uint32_t kbps : {384u, 448u}) {
        CAPTURE(kbps);
        ac3::EncoderConfig config;
        config.acmod = layout->acmod;
        config.lfe = layout->lfe;
        config.bitrate_kbps = kbps;
        const auto failure = mirror_encode(config, channels);
        INFO(failure);
        CHECK(failure.empty());
    }
}

TEST_CASE("the mirror check is off unless a trace is attached", "[verify]") {
    ac3::EncoderConfig config;
    config.acmod = ac3::Acmod::k2_0;
    CHECK(config.trace == nullptr);
    ac3::DecoderConfig decoder_config;
    CHECK(decoder_config.trace == nullptr);

    // And attaching one changes nothing about the output: the trace reads
    // state the encoder already has, it never steers a decision. A regression
    // here would make the checked build a different encoder from the shipped
    // one, which would make the check worthless.
    const auto channels = golden_audio("reference_stereo.wav");
    std::vector<std::span<const float>> views{std::span{channels[0]}.first(ac3::kSamplesPerFrame),
                                              std::span{channels[1]}.first(ac3::kSamplesPerFrame)};

    ac3::FrameEncoder plain{config};
    const auto without = plain.encode_frame(views);
    REQUIRE(without.has_value());

    ac3::verify::FrameTrace trace;
    config.trace = &trace;
    ac3::FrameEncoder traced{config};
    const auto with = traced.encode_frame(views);
    REQUIRE(with.has_value());

    CHECK(*without == *with);
    CHECK(trace.fbw_channels == 2);
    CHECK(trace.coded_channels == 2);
    CHECK(trace.blocks.front().entered);
    CHECK(trace.blocks.back().allocated);
}
