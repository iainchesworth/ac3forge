#include <catch2/catch_test_macros.hpp>

#include <spa/utils/dict.h>

#include <string>

#include "pipewire_support.hpp"

// The PipeWire backend's pure half, tested with no PipeWire session running
// at all - CMake adds this file to the suite only when it selected the
// pipewire/ platform directory (the same axis tests/platform/alsa/'s own
// file rides), and puts that directory on the include path.
//
// What is worth testing here is what does not need a session to get wrong:
// the carrier-rate/codec mapping (miss this and a burst stream announces
// the wrong link speed or codec, the same class of silent failure
// test_alsa_device_names.cpp exists to catch for ALSA's channel status), and
// the property-dictionary reads capture.cpp/passthrough.cpp use to classify
// and name a node. A spa_dict is a plain struct - no libpipewire connection,
// daemon or hardware needed to build one by hand.

using ac3::pipewire::carrier_rate;
using ac3::pipewire::iec958_codec_for;
using ac3::pipewire::is_audio_sink;
using ac3::pipewire::is_audio_source;
using ac3::pipewire::node_friendly_name;
using ac3::pipewire::node_id;
using ac3::audio::BitstreamFormat;

namespace {

spa_dict make_dict(const spa_dict_item* items, std::uint32_t n_items) {
    // Plain aggregate init, not the SPA_DICT_INIT macro: it expands to a C99
    // compound literal, which -Werror -Wc99-extensions (Clang) flags outside
    // the system header that otherwise shelters it - see
    // src/audio/src/backend/pipewire/capture.cpp's identical note on
    // SPA_POD_BUILDER_INIT.
    return spa_dict{0, n_items, items};
}

}  // namespace

TEST_CASE("E-AC-3 runs the link four times as fast as its content, same as ALSA") {
    // The PipeWire-side twin of ac3::alsa::carrier_rate() - see
    // pipewire_support.hpp's comment on why it is a second three-line
    // function rather than a cross-backend helper.
    CHECK(carrier_rate(BitstreamFormat::kAc3, 48000) == 48000);
    CHECK(carrier_rate(BitstreamFormat::kAc3, 44100) == 44100);
    CHECK(carrier_rate(BitstreamFormat::kAc3, 32000) == 32000);
    CHECK(carrier_rate(BitstreamFormat::kEac3, 48000) == 192000);
    CHECK(carrier_rate(BitstreamFormat::kEac3, 44100) == 176400);
    CHECK(carrier_rate(BitstreamFormat::kEac3, 32000) == 128000);
}

TEST_CASE("the codec sent over IEC958 matches the bitstream format asked for") {
    CHECK(iec958_codec_for(BitstreamFormat::kAc3) == SPA_AUDIO_IEC958_CODEC_AC3);
    CHECK(iec958_codec_for(BitstreamFormat::kEac3) == SPA_AUDIO_IEC958_CODEC_EAC3);
}

TEST_CASE("a node is classified as a source, a sink, or neither") {
    const spa_dict_item source_items[] = {{PW_KEY_MEDIA_CLASS, "Audio/Source"}};
    const auto source = make_dict(source_items, 1);
    CHECK(is_audio_source(source));
    CHECK_FALSE(is_audio_sink(source));

    const spa_dict_item sink_items[] = {{PW_KEY_MEDIA_CLASS, "Audio/Sink"}};
    const auto sink = make_dict(sink_items, 1);
    CHECK(is_audio_sink(sink));
    CHECK_FALSE(is_audio_source(sink));

    // Some other class entirely (video, a stream, a virtual device) is
    // neither - silently not visited by either enumerate_devices() or
    // enumerate_render_devices(), not misclassified into one of them.
    const spa_dict_item video_items[] = {{PW_KEY_MEDIA_CLASS, "Video/Source"}};
    const auto video = make_dict(video_items, 1);
    CHECK_FALSE(is_audio_source(video));
    CHECK_FALSE(is_audio_sink(video));

    const spa_dict empty = make_dict(nullptr, 0);
    CHECK_FALSE(is_audio_source(empty));
    CHECK_FALSE(is_audio_sink(empty));
}

TEST_CASE("a node's id is its node.name - what PW_KEY_TARGET_OBJECT actually accepts") {
    const spa_dict_item items[] = {{PW_KEY_NODE_NAME, "alsa_output.pci-0000_00_1f.3.analog-stereo"},
                                    {PW_KEY_MEDIA_CLASS, "Audio/Sink"}};
    const auto dict = make_dict(items, 2);
    CHECK(node_id(dict) == "alsa_output.pci-0000_00_1f.3.analog-stereo");

    const spa_dict empty = make_dict(nullptr, 0);
    CHECK(node_id(empty).empty());
}

TEST_CASE("a node's friendly name prefers node.description over node.name") {
    const spa_dict_item both[] = {{PW_KEY_NODE_NAME, "alsa_output.raw"},
                                   {PW_KEY_NODE_DESCRIPTION, "Built-in Audio Analog Stereo"}};
    CHECK(node_friendly_name(make_dict(both, 2)) == "Built-in Audio Analog Stereo");

    // No description - the id is the only name there is to show.
    const spa_dict_item name_only[] = {{PW_KEY_NODE_NAME, "alsa_output.raw"}};
    CHECK(node_friendly_name(make_dict(name_only, 1)) == "alsa_output.raw");

    // An empty description is the same as none - PipeWire nodes have been
    // seen to advertise one, and showing a blank name would be worse than
    // falling back.
    const spa_dict_item blank_description[] = {{PW_KEY_NODE_NAME, "alsa_output.raw"},
                                                {PW_KEY_NODE_DESCRIPTION, ""}};
    CHECK(node_friendly_name(make_dict(blank_description, 2)) == "alsa_output.raw");
}
