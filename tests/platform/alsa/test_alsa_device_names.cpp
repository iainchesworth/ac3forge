#include <catch2/catch_test_macros.hpp>

#include <string>

#include "device_names.hpp"

// The ALSA backend's pure half, tested on a machine with no sound card.
//
// CMake adds this file to the suite only when it selected the alsa/ platform
// directory, and puts that directory on the include path - the same selection
// that decides which backend the library gets, so a build cannot end up
// testing a backend it did not compile. tests/platform/ already worked this
// way for the CRT report hook; this is the second axis to use it.
//
// What is worth testing here is what cannot be tested any other way. Opening a
// device needs hardware; deciding WHICH device to open, and with what channel
// status, does not - and it is where a mistake is expensive and silent. An
// AC-3 burst stream sent with the non-audio bit clear does not fail: the
// receiver believes it and reproduces a 6144-byte-per-frame bit pattern as
// full-scale noise. Nothing downstream catches that, so it is caught here.

using ac3::alsa::ChannelStatus;
using ac3::alsa::classify_digital_output;
using ac3::alsa::config_device_name;
using ac3::alsa::DigitalOutput;
using ac3::alsa::has_channel_status_args;
using ac3::alsa::non_audio_channel_status;
using ac3::alsa::passthrough_device_name;
using ac3::alsa::takes_channel_status_args;

TEST_CASE("non-audio channel status sets the bit that makes a receiver decode") {
    const auto status = non_audio_channel_status(48000);
    REQUIRE(status.has_value());

    // IEC958_AES0_NONAUDIO. Everything else in this file is convenience; this
    // is the one bit that decides between Dolby Digital and noise.
    CHECK((status->aes0 & 0x02) != 0);
    // Consumer format, not professional: bit 0 clear.
    CHECK((status->aes0 & 0x01) == 0);
    // No pre-emphasis: bits 3..5 clear.
    CHECK((status->aes0 & 0x38) == 0);
}

TEST_CASE("channel status carries the rate the link is actually running at") {
    // IEC958_AES3_CON_FS_*, in AES3 bits 0..3. A receiver locks its clock from
    // these, so a 44.1 kHz link announced as 48 kHz does not merely play
    // slightly fast - it fails to lock.
    CHECK(non_audio_channel_status(44100)->aes3 == 0x00);
    CHECK(non_audio_channel_status(48000)->aes3 == 0x02);
    CHECK(non_audio_channel_status(32000)->aes3 == 0x03);
    // The 4x links E-AC-3 needs.
    CHECK(non_audio_channel_status(176400)->aes3 == 0x0C);
    CHECK(non_audio_channel_status(192000)->aes3 == 0x0E);

    // Those five are every rate this backend can ever be asked for: AC-3's
    // three content rates (A/52 §5.4.1.3 defines fscod for 48, 44.1 and 32 kHz
    // and nothing else) and the two of their 4x carriers that IEC 60958 has a
    // consumer code for.
    CHECK_FALSE(non_audio_channel_status(96000).has_value());
    CHECK_FALSE(non_audio_channel_status(0).has_value());
    // The gap: E-AC-3 at 32 kHz would want a 128 kHz link, and there is no
    // consumer frequency code for one. Refused, not approximated - see the
    // round-trip case at the end of this file for what the sink does with it.
    CHECK_FALSE(non_audio_channel_status(128000).has_value());
}

TEST_CASE("E-AC-3 runs the link four times as fast as its content") {
    using ac3::alsa::carrier_rate;
    using ac3::audio::BitstreamFormat;

    // AC-3 carries at the content rate...
    CHECK(carrier_rate(BitstreamFormat::kAc3, 48000) == 48000);
    CHECK(carrier_rate(BitstreamFormat::kAc3, 44100) == 44100);
    CHECK(carrier_rate(BitstreamFormat::kAc3, 32000) == 32000);
    // ...and E-AC-3 at four times it, because its burst is four times the size
    // and covers the same span of time.
    CHECK(carrier_rate(BitstreamFormat::kEac3, 48000) == 192000);
    CHECK(carrier_rate(BitstreamFormat::kEac3, 44100) == 176400);
    CHECK(carrier_rate(BitstreamFormat::kEac3, 32000) == 128000);
}

TEST_CASE("the whole status matches what alsa-lib's own iec958 device defaults to") {
    // alsa-lib's /usr/share/alsa/pcm/iec958.conf defaults to AES0=0x04,
    // AES1=0x82, AES2=0x00, AES3=0x02 - consumer, not-copyright, PCM coder,
    // original, 48 kHz. Ours is that exactly, plus the non-audio bit. Pinning
    // the full value here so a change to any of the other bytes is a decision
    // somebody made rather than a typo nobody noticed.
    CHECK(non_audio_channel_status(48000) ==
          ChannelStatus{.aes0 = 0x06, .aes1 = 0x82, .aes2 = 0x00, .aes3 = 0x02});
}

TEST_CASE("only the plugins that take AES arguments are given them") {
    CHECK(takes_channel_status_args("iec958"));
    CHECK(takes_channel_status_args("iec958:CARD=PCH,DEV=0"));
    CHECK(takes_channel_status_args("hdmi"));
    CHECK(takes_channel_status_args("hdmi:CARD=HDMI,DEV=1"));

    // A raw device takes CARD/DEV/SUBDEV and would refuse a fifth argument, so
    // appending channel status to one produces a name that will not open.
    CHECK_FALSE(takes_channel_status_args("hw:CARD=PCH,DEV=1"));
    CHECK_FALSE(takes_channel_status_args("plughw:0,3"));
    CHECK_FALSE(takes_channel_status_args("default"));
    CHECK_FALSE(takes_channel_status_args("pulse"));
    // Not a prefix match on the plugin name: "iec958x" is some other device.
    CHECK_FALSE(takes_channel_status_args("iec958x:CARD=PCH"));
    CHECK_FALSE(takes_channel_status_args(""));
}

TEST_CASE("a digital device name gains the channel status arguments") {
    const auto name = passthrough_device_name("iec958:CARD=PCH,DEV=0", 48000);
    REQUIRE(name.has_value());
    CHECK(*name == "iec958:CARD=PCH,DEV=0,AES0=0x06,AES1=0x82,AES2=0x00,AES3=0x02");
    CHECK(has_channel_status_args(*name));

    const auto hdmi = passthrough_device_name("hdmi:CARD=HDMI,DEV=2", 44100);
    REQUIRE(hdmi.has_value());
    CHECK(*hdmi == "hdmi:CARD=HDMI,DEV=2,AES0=0x06,AES1=0x82,AES2=0x00,AES3=0x00");
}

TEST_CASE("a name that already carries channel status is left alone") {
    // The escape hatch: a caller who wrote the AES bytes by hand meant them,
    // including a professional-format or emphasised stream this backend would
    // never construct.
    const std::string handwritten = "iec958:CARD=PCH,DEV=0,AES0=0x03,AES1=0x00,AES2=0x00,AES3=0x02";
    const auto name = passthrough_device_name(handwritten, 48000);
    REQUIRE(name.has_value());
    CHECK(*name == handwritten);

    // Even at a rate that has no code of its own - the caller supplied one.
    CHECK(passthrough_device_name(handwritten, 96000) == handwritten);
}

TEST_CASE("a raw device name is passed through untouched") {
    // Not an oversight: hw: cannot take the arguments, and its channel status
    // comes from the card's "IEC958 Playback Default" control instead. The
    // caller who named a raw device owns that.
    CHECK(passthrough_device_name("hw:CARD=PCH,DEV=1", 48000) == "hw:CARD=PCH,DEV=1");
    CHECK(passthrough_device_name("plughw:0,3", 48000) == "plughw:0,3");
}

TEST_CASE("an unusable rate is refused rather than guessed at") {
    // Only for a name that would have carried the status - a raw device is
    // still returned above, because the rate was never ours to encode there.
    CHECK_FALSE(passthrough_device_name("iec958:CARD=PCH,DEV=0", 96000).has_value());
    CHECK_FALSE(passthrough_device_name("", 48000).has_value());
}

TEST_CASE("E-AC-3 at 32 kHz has nowhere to go, and says so") {
    using ac3::alsa::carrier_rate;
    using ac3::audio::BitstreamFormat;

    // The one combination this library can encode and this backend cannot
    // carry, followed all the way through as PassthroughSink::start() does it:
    // a 128 kHz link with no IEC 60958 frequency code, so no device name, so
    // kFormatRejected. Announcing some other rate instead would produce a
    // stream a receiver locks onto and then decodes at the wrong speed.
    const auto link = carrier_rate(BitstreamFormat::kEac3, 32000);
    CHECK(link == 128000);
    CHECK_FALSE(passthrough_device_name("hdmi:CARD=HDMI,DEV=0", link).has_value());

    // Its 48 and 44.1 kHz siblings are fine.
    CHECK(passthrough_device_name("hdmi:CARD=HDMI,DEV=0",
                                  carrier_rate(BitstreamFormat::kEac3, 48000)) ==
          "hdmi:CARD=HDMI,DEV=0,AES0=0x06,AES1=0x82,AES2=0x00,AES3=0x0e");
}

TEST_CASE("digital outputs are recognised through the spellings drivers use") {
    CHECK(classify_digital_output("HDMI 0") == DigitalOutput::kHdmi);
    CHECK(classify_digital_output("HDMI/DP,pcm=3") == DigitalOutput::kHdmi);
    CHECK(classify_digital_output("hdmi 7") == DigitalOutput::kHdmi);
    CHECK(classify_digital_output("DisplayPort Audio") == DigitalOutput::kHdmi);

    CHECK(classify_digital_output("ALC1220 Digital") == DigitalOutput::kSpdif);
    CHECK(classify_digital_output("IEC958") == DigitalOutput::kSpdif);
    CHECK(classify_digital_output("iec958") == DigitalOutput::kSpdif);
    CHECK(classify_digital_output("SPDIF Out") == DigitalOutput::kSpdif);
    CHECK(classify_digital_output("S/PDIF Optical") == DigitalOutput::kSpdif);

    CHECK(classify_digital_output("ALC295 Analog") == DigitalOutput::kNone);
    CHECK(classify_digital_output("USB Audio") == DigitalOutput::kNone);
    CHECK(classify_digital_output("") == DigitalOutput::kNone);
}

TEST_CASE("an HDMI PCM that also says digital is classified as HDMI") {
    // Both markers are present and only one of them names a plugin that can
    // open it, so the more specific answer has to win.
    CHECK(classify_digital_output("HDMI 0 Digital Out") == DigitalOutput::kHdmi);
}

TEST_CASE("a PCM with no marker of its own falls back to the card's name") {
    // vc4-hdmi (Raspberry Pi's HDMI output) names every PCM identically -
    // "MAI PCM i2s-hifi-0" - regardless of which HDMI port it is; "hdmi" only
    // ever shows up in the card's own id ("vc4hdmi0") and name ("vc4-hdmi-0").
    // Found live: a real receiver connected and ELD-populated, and 'ac3cli
    // outputs' still reported no render endpoints until this fallback existed.
    CHECK(classify_digital_output("MAI PCM i2s-hifi-0", "vc4hdmi0", "vc4-hdmi-0") ==
          DigitalOutput::kHdmi);
    CHECK(classify_digital_output("MAI PCM i2s-hifi-0", "vc4hdmi1", "vc4-hdmi-1") ==
          DigitalOutput::kHdmi);

    // The device name is still checked first: a driver that names the PCM
    // itself doesn't need the card to say anything.
    CHECK(classify_digital_output("HDMI 0", "PCH", "HDA Intel PCH") == DigitalOutput::kHdmi);

    // A genuinely analog device stays kNone even though the fallback ran -
    // nothing in "bcm2835 Headphones" (device, id, or name) says digital.
    CHECK(classify_digital_output("bcm2835 Headphones", "Headphones", "bcm2835 Headphones") ==
          DigitalOutput::kNone);
}

TEST_CASE("a configuration device name indexes the plugin, not the hardware") {
    // hdmi:DEV=n is the card's n-th HDMI PCM, which on an HDA card is hardware
    // device 3, 7, 8... - so this must never be built from the hw device
    // number. The two spellings sit side by side here to keep that visible.
    CHECK(config_device_name(DigitalOutput::kHdmi, "HDMI", 0) == "hdmi:CARD=HDMI,DEV=0");
    CHECK(config_device_name(DigitalOutput::kHdmi, "HDMI", 3) == "hdmi:CARD=HDMI,DEV=3");
    CHECK(config_device_name(DigitalOutput::kSpdif, "PCH", 0) == "iec958:CARD=PCH,DEV=0");
    CHECK(ac3::alsa::hw_device_name("PCH", 1) == "hw:CARD=PCH,DEV=1");
}

TEST_CASE("a candidate device name survives the round trip to an openable one") {
    // The two halves as the sink uses them: enumeration publishes a base name,
    // start() turns it into the name snd_pcm_open() is given. What comes out
    // has to still be the device that went in.
    const std::string base = config_device_name(DigitalOutput::kSpdif, "PCH", 0);
    const auto opened = passthrough_device_name(base, 48000);
    REQUIRE(opened.has_value());
    CHECK(opened->starts_with(base));
    CHECK(has_channel_status_args(*opened));
}
