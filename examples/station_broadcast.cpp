// A fully worked scene: a space station broadcasting its anthem, encoded as
// Dolby Atmos in Dolby Digital Plus (ETSI TS 103 420).
//
// The premise is borrowed lovingly from the title sequence of Star Trek:
// Deep Space Nine (and from the fan edit that re-mixed its theme as if the
// music were coming FROM the station). Every sound here is diegetic: the
// anthem is a radio broadcast from the station, and everything else in the
// scene - a passing comet, a cargo jalopy with its own radio on, two
// runabouts, a welding pod, and finally the wormhole - is an Atmos object
// with an authored flight path that ducks, swamps, and frames that broadcast
// the way the picture would.
//
// The 115-second cue sheet (times are scene-relative; the two reference
// videos start the sequence at 0:01 and 0:14 respectively):
//
//     0:00  a comet drifts across the black, left to right
//     0:02  the station's broadcast fades up - tinny, distant, mono
//     0:14  the station pans into view; the signal firms up
//     0:26  cargo jalopy flypast, engine rumble and someone's boogie
//           radio drowning the anthem as it crosses the right side
//     0:38  cut to a station close-up: the broadcast opens to full
//           bandwidth and takes the room
//     0:52  a runabout undocks and sweeps overhead, squawking the tower
//     1:01  the anthem surges back for the reprise
//     1:12  a second runabout crosses the port side
//     1:24  a maintenance pod welds sparks off an upper pylon
//     1:43  the wormhole opens behind the station - subsonic bloom,
//           shimmer wrapping up and over the room - and, as in the
//           original cue, the music's final chord lands right on it
//     1:51  ...and swallows itself
//
// The built-in anthem is a synthesizer COVER of the Deep Space Nine main
// title (Dennis McCarthy) - the seasons 1-3 arrangement, by ear: C major,
// the solo horn call over a pad, the solo-trumpet fanfare, and a final
// open-fifth cadence timed (as in the original) to the wormhole. The middle
// section was never transcribed anywhere, so it is recomposed here from the
// theme's own material. Distributing a cover needs a licence wherever you
// distribute it; this repository's author has secured their own position,
// and yours is your own affair. To hear the scene with a recording you own
// instead, pass its WAV as the third argument and the broadcast plays that,
// starting at scene time 0:02.
//
// Usage:
//     station_broadcast                          smoke test: renders 0:24-0:34
//                                                in memory, writes nothing
//     station_broadcast out [seconds] [wav]      full render, writes:
//         out.ec3          E-AC-3 with OAMD+JOC objects (Atmos)
//         out_bed51.ec3    same mix, no EMDF container - for decoders that
//                          validate emdf_protection (see ac3/oba/atmos.hpp)
//         out_bed.wav      the 5.1 float bed a legacy decoder would hear
//         out_stereo.wav   a Lo/Ro headphone downmix of that bed

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <numbers>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/io/wav.hpp"
#include "ac3/oba/atmos.hpp"
#include "ac3/oba/motion.hpp"

namespace {

constexpr double kRate = 48000.0;
constexpr double kDt = 1.0 / kRate;
constexpr double kPi = std::numbers::pi;
constexpr double kTau = 2.0 * kPi;

// ---------------------------------------------------------------------------
// A tiny deterministic DSP kit. The library ships panners and paths, not
// synthesizers; everything below exists so this example needs no assets.
// ---------------------------------------------------------------------------

// xorshift64*: deterministic across platforms, so two runs (and two CI hosts)
// render bit-identical essences.
struct Rng {
    std::uint64_t state;
    explicit Rng(std::uint64_t seed) : state(seed | 1u) {}
    double next01() {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return static_cast<double>((state * 0x2545F4914F6CDD1Dull) >> 11) * 0x1.0p-53;
    }
    double bipolar() { return 2.0 * next01() - 1.0; }
};

struct OnePole {
    double a = 1.0;
    double y = 0.0;
    void set_cutoff(double hz) { a = 1.0 - std::exp(-kTau * hz * kDt); }
    double process(double x) {
        y += a * (x - y);
        return y;
    }
};

// RBJ biquad, transposed direct form II.
struct Biquad {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double z1 = 0.0, z2 = 0.0;

    static Biquad lowpass(double hz, double q) {
        Biquad f;
        const double w = kTau * hz * kDt;
        const double alpha = std::sin(w) / (2.0 * q);
        const double c = std::cos(w);
        const double a0 = 1.0 + alpha;
        f.b0 = (1.0 - c) / 2.0 / a0;
        f.b1 = (1.0 - c) / a0;
        f.b2 = f.b0;
        f.a1 = -2.0 * c / a0;
        f.a2 = (1.0 - alpha) / a0;
        return f;
    }
    static Biquad highpass(double hz, double q) {
        Biquad f;
        const double w = kTau * hz * kDt;
        const double alpha = std::sin(w) / (2.0 * q);
        const double c = std::cos(w);
        const double a0 = 1.0 + alpha;
        f.b0 = (1.0 + c) / 2.0 / a0;
        f.b1 = -(1.0 + c) / a0;
        f.b2 = f.b0;
        f.a1 = -2.0 * c / a0;
        f.a2 = (1.0 - alpha) / a0;
        return f;
    }
    double process(double x) {
        const double out = b0 * x + z1;
        z1 = b1 * x - a1 * out + z2;
        z2 = b2 * x - a2 * out;
        return out;
    }
};

// polyBLEP: enough anti-aliasing for a fanfare, in six lines.
double poly_blep(double t, double inc) {
    if (t < inc) {
        const double x = t / inc;
        return x + x - x * x - 1.0;
    }
    if (t > 1.0 - inc) {
        const double x = (t - 1.0) / inc;
        return x * x + x + x + 1.0;
    }
    return 0.0;
}

double blep_saw(double& phase, double inc) {
    phase += inc;
    if (phase >= 1.0) {
        phase -= 1.0;
    }
    return 2.0 * phase - 1.0 - poly_blep(phase, inc);
}

// ---------------------------------------------------------------------------
// The anthem. A note list rendered by a handful of subtractive voices - the
// "asset" of this example is a score, not a sample.
// ---------------------------------------------------------------------------

enum class Voice : std::uint8_t {
    kHorn,
    kTrumpet,
    kPad,
    kTimpani,
    kHarp,
    kCymbal,
    kBoogieLead,
    kBoogieBass,
};

struct Note {
    double t = 0.0;
    double dur = 0.0;
    double hz = 440.0;
    double vel = 0.5;
    Voice voice = Voice::kHorn;
};

double midi_hz(int midi) { return 440.0 * std::pow(2.0, (midi - 69) / 12.0); }

struct ScoreBuilder {
    std::vector<Note> notes;

    void note(double t, double dur, int midi, double vel, Voice voice) {
        notes.push_back({t, dur, midi_hz(midi), vel, voice});
    }
    void chord(double t, double dur, std::initializer_list<int> midis, double vel) {
        for (const int m : midis) {
            note(t, dur, m, vel, Voice::kPad);
        }
    }
    // A timpani roll: strokes every 55 ms, velocity ramped v0 -> v1.
    void roll(double t, double dur, int midi, double v0, double v1) {
        for (double s = 0.0; s < dur; s += 0.055) {
            note(t + s, 0.09, midi, v0 + (v1 - v0) * (s / dur), Voice::kTimpani);
        }
    }
};

// A harp figure: sequential plucks, each ringing past the next.
void arp(ScoreBuilder& s, double t, std::initializer_list<int> midis, double step, double vel) {
    double at = t;
    for (const int m : midis) {
        s.note(at, step * 1.9, m, vel, Voice::kHarp);
        at += step;
    }
}

// The opening solo horn call, over the pad: a two-note rising fourth held
// long, a quarter-note ascent spanning the octave to a held high note under
// the string swell, and a stepwise descent to a dark landing.
void horn_call(ScoreBuilder& s) {
    s.note(5.5, 0.45, 62, 0.42, Voice::kHorn);   // D4 pickup
    s.note(5.95, 3.35, 67, 0.48, Voice::kHorn);  // G4, held across the bar
    s.note(9.6, 1.0, 62, 0.45, Voice::kHorn);    // the ascent: D G A D'
    s.note(10.6, 1.0, 67, 0.47, Voice::kHorn);
    s.note(11.6, 1.0, 69, 0.50, Voice::kHorn);
    s.note(12.6, 3.6, 74, 0.55, Voice::kHorn);   // D5 under the string swell
    s.note(16.2, 1.0, 72, 0.50, Voice::kHorn);   // the descent: C B G, to C
    s.note(17.2, 1.0, 71, 0.48, Voice::kHorn);
    s.note(18.2, 1.0, 67, 0.45, Voice::kHorn);
    s.note(19.2, 1.4, 60, 0.42, Voice::kHorn);
    // The string swell that carries the call's high note.
    s.chord(15.6, 3.4, {72, 79}, 0.26);
    s.chord(16.6, 2.4, {72, 79}, 0.36);
}

// One statement of the main fanfare (solo trumpet, per the S1-3 cue): the
// quick 5-1-3 pickup into the held second degree, answered by the 4-4-3-4
// figure cresting on a held fifth, over C | F | G harmony.
void statement(ScoreBuilder& s, double t0, double vel, bool doubled, bool continuation) {
    // The tune (concert C): quick pickup G4 C5 E5, D5 held; answer F5 F5
    // E5 F5 cresting on a held G5. Body pulse ~75 bpm (0.8 s beats).
    s.note(t0, 0.27, 67, vel, Voice::kTrumpet);
    s.note(t0 + 0.27, 0.27, 72, vel, Voice::kTrumpet);
    s.note(t0 + 0.54, 0.28, 76, vel, Voice::kTrumpet);
    s.note(t0 + 0.82, 2.40, 74, vel, Voice::kTrumpet);
    s.note(t0 + 3.60, 0.55, 77, vel, Voice::kTrumpet);
    s.note(t0 + 4.15, 0.55, 77, vel, Voice::kTrumpet);
    s.note(t0 + 4.70, 0.27, 76, vel, Voice::kTrumpet);
    s.note(t0 + 4.97, 0.28, 77, vel, Voice::kTrumpet);
    s.note(t0 + 5.25, 2.55, 79, vel, Voice::kTrumpet);
    if (doubled) {
        // Horns shadow the held notes an octave down for the big statements.
        s.note(t0 + 0.82, 2.4, 62, vel * 0.6, Voice::kHorn);
        s.note(t0 + 5.25, 2.5, 67, vel * 0.6, Voice::kHorn);
    }

    s.chord(t0, 3.2, {36, 48, 55, 64}, vel * 0.8);
    s.chord(t0 + 3.2, 3.2, {41, 53, 60, 65}, vel * 0.8);
    s.chord(t0 + 6.4, 1.6, {43, 55, 59, 62}, vel * 0.75);
    s.note(t0, 0.8, 36, vel * 0.9, Voice::kTimpani);
    s.note(t0 + 3.2, 0.8, 41, vel * 0.85, Voice::kTimpani);
    s.note(t0 + 6.4, 0.8, 43, vel * 0.85, Voice::kTimpani);

    if (continuation) {
        // Settling tail: down from the crest and home to C.
        s.note(t0 + 8.0, 0.5, 77, vel * 0.9, Voice::kTrumpet);
        s.note(t0 + 8.5, 0.5, 76, vel * 0.85, Voice::kTrumpet);
        s.note(t0 + 9.0, 0.9, 72, vel * 0.85, Voice::kTrumpet);
        s.note(t0 + 9.9, 1.7, 67, vel * 0.75, Voice::kTrumpet);
        s.chord(t0 + 8.0, 1.6, {41, 53, 57, 65}, vel * 0.7);
        s.chord(t0 + 9.6, 2.0, {36, 48, 55, 60}, vel * 0.75);
        s.note(t0 + 9.6, 0.8, 36, vel * 0.8, Voice::kTimpani);
    } else {
        s.chord(t0 + 8.0, 1.6, {36, 48, 55, 60, 67}, vel * 0.8);
    }
}

// The full 115-second cover, mapped to the measured form of the S1-3 cue
// (scene time = cue time + 1 s; the picture's cut points and the score's
// section boundaries land where the original's do, cadence included).
std::vector<Note> compose_cover() {
    ScoreBuilder s;

    // 0:02-0:28 - the tonic drone with slow arpeggios on C, Csus4, and G:
    // the "major chords over a major root drone" opening.
    s.chord(2.0, 26.5, {36, 43, 48}, 0.4);
    for (const double base : {2.6, 12.6}) {
        arp(s, base, {48, 52, 55, 60}, 0.55, 0.35);
        arp(s, base + 2.5, {48, 53, 55, 60}, 0.55, 0.35);
        arp(s, base + 5.0, {48, 52, 55, 60}, 0.55, 0.35);
        arp(s, base + 7.5, {43, 47, 50, 55}, 0.55, 0.35);
    }
    arp(s, 22.6, {48, 52, 55, 60}, 0.55, 0.30);
    arp(s, 25.1, {48, 53, 55, 60}, 0.55, 0.28);

    // 0:05.5-0:20.5 - the solo call.
    horn_call(s);

    // 0:21-0:28 - hushed, sparse strings alone.
    s.chord(21.0, 7.5, {60, 67}, 0.22);

    // 0:28.5 - the full orchestra arrives at a step (the ~10 dB entry in
    // the original): introductory fanfare chords ahead of the theme.
    s.note(27.4, 1.1, 60, 0.50, Voice::kCymbal);
    s.roll(27.9, 0.6, 36, 0.25, 0.6);
    s.chord(28.5, 1.6, {36, 48, 55, 60, 64, 67}, 0.85);
    s.note(28.5, 1.6, 60, 0.60, Voice::kHorn);
    s.note(28.5, 1.6, 64, 0.55, Voice::kHorn);
    s.note(28.5, 0.9, 36, 0.80, Voice::kTimpani);
    s.chord(30.1, 1.6, {36, 53, 57, 60, 65}, 0.80);
    s.note(30.1, 0.8, 41, 0.70, Voice::kTimpani);
    s.chord(31.7, 1.3, {43, 50, 55, 60, 62}, 0.85);
    s.note(31.7, 1.3, 67, 0.60, Voice::kHorn);
    s.note(31.7, 0.8, 43, 0.75, Voice::kTimpani);

    // 0:33 - the main theme statement, solo trumpet, on the title card.
    // Its crest rings straight across the picture's close-up cut at 0:38.
    statement(s, 33.0, 0.7, false, true);

    // 0:44.6-0:60.5 - the quieter middle. No source transcribes this
    // stretch, so it is recomposed from the theme's own descent shapes;
    // strings lead, quietest under the runabout pass.
    s.chord(44.6, 3.2, {50, 57, 60, 65}, 0.40);
    s.chord(47.8, 3.2, {52, 55, 60}, 0.38);
    s.chord(51.0, 3.2, {41, 53, 57, 65}, 0.40);
    s.chord(54.2, 3.2, {48, 55, 62}, 0.35);
    s.chord(57.4, 3.0, {43, 55, 60}, 0.35);
    s.note(44.6, 1.6, 69, 0.35, Voice::kHorn);
    s.note(46.2, 1.6, 67, 0.34, Voice::kHorn);
    s.note(47.8, 2.4, 64, 0.33, Voice::kHorn);
    s.note(50.2, 0.8, 62, 0.32, Voice::kHorn);
    s.note(51.0, 1.6, 65, 0.34, Voice::kHorn);
    s.note(52.6, 1.6, 64, 0.32, Voice::kHorn);
    s.note(54.2, 2.4, 62, 0.30, Voice::kHorn);
    s.note(56.6, 3.4, 60, 0.28, Voice::kHorn);
    arp(s, 53.5, {48, 55, 60}, 0.7, 0.22);
    arp(s, 56.2, {48, 55, 60, 67}, 0.7, 0.20);

    // 1:01 - the reprise re-enters sharply.
    s.note(59.9, 1.1, 60, 0.55, Voice::kCymbal);
    s.roll(60.4, 0.6, 36, 0.3, 0.7);
    s.chord(61.0, 1.5, {36, 48, 55, 60, 64, 67}, 0.90);
    s.note(61.0, 0.9, 36, 0.85, Voice::kTimpani);
    statement(s, 61.8, 0.8, true, true);
    // Bridge into the plateau: horns climbing stepwise.
    s.note(73.4, 1.6, 64, 0.55, Voice::kHorn);
    s.note(75.0, 1.6, 65, 0.58, Voice::kHorn);
    s.note(76.6, 1.6, 67, 0.60, Voice::kHorn);
    s.note(78.2, 0.8, 69, 0.62, Voice::kHorn);
    s.note(79.0, 0.9, 71, 0.65, Voice::kHorn);
    s.chord(73.4, 3.2, {41, 53, 57, 65}, 0.55);
    s.chord(76.6, 1.6, {43, 50, 55, 62}, 0.60);
    s.chord(78.2, 1.7, {43, 55, 60}, 0.60);
    s.note(76.6, 0.5, 43, 0.50, Voice::kTimpani);
    s.note(78.2, 0.5, 43, 0.55, Voice::kTimpani);
    s.note(79.2, 0.4, 43, 0.60, Voice::kTimpani);

    // 1:19-1:42 - the climax plateau: two more full statements with horn
    // doubling and harp runs, then sustained open fifths.
    s.note(78.9, 1.1, 60, 0.60, Voice::kCymbal);
    arp(s, 79.2, {48, 52, 55, 60, 64, 67, 72, 76}, 0.14, 0.30);
    statement(s, 80.0, 0.85, true, false);
    s.chord(88.0, 1.6, {36, 43, 48, 55, 60, 67}, 0.80);
    s.note(88.0, 3.2, 60, 0.50, Voice::kHorn);
    s.note(88.0, 3.2, 67, 0.50, Voice::kHorn);
    s.note(88.5, 1.1, 60, 0.65, Voice::kCymbal);
    arp(s, 88.8, {48, 52, 55, 60, 64, 67, 72, 76}, 0.14, 0.32);
    statement(s, 89.6, 0.9, true, true);
    s.chord(101.2, 2.1, {36, 43, 48, 55}, 0.60);
    s.note(101.2, 2.0, 60, 0.55, Voice::kHorn);
    s.note(101.2, 2.0, 67, 0.55, Voice::kHorn);
    s.note(101.2, 2.0, 72, 0.50, Voice::kTrumpet);
    for (int i = 0; i < 4; ++i) {
        s.note(100.0 + 0.8 * i, 0.4, 36, 0.5 + 0.05 * i, Voice::kTimpani);
    }

    // 1:43.5 - the two-beat breath...
    s.roll(103.5, 0.45, 36, 0.15, 0.4);

    // 1:44 - ...and the final cadence, an open fifth (the lead sheet closes
    // on a C5 chord symbol), landing with the wormhole and sustaining while
    // it blooms.
    s.note(103.6, 0.4, 60, 0.70, Voice::kCymbal);
    s.chord(104.0, 5.8, {36, 43, 48, 55, 60, 67, 72, 79}, 0.85);
    s.note(104.0, 5.0, 60, 0.70, Voice::kHorn);
    s.note(104.0, 5.0, 67, 0.70, Voice::kHorn);
    s.note(104.0, 4.5, 72, 0.65, Voice::kTrumpet);
    s.note(104.0, 4.0, 79, 0.45, Voice::kTrumpet);
    s.roll(104.0, 2.8, 36, 0.5, 0.8);
    // Afterglow as the wormhole swallows itself.
    arp(s, 110.3, {48, 55, 60, 67, 72}, 0.4, 0.18);
    s.chord(110.5, 3.2, {36, 43, 48}, 0.28);

    return std::move(s.notes);
}

// What the jalopy has on: seven bars of boogie in F, 132 bpm, swung hard
// enough to survive a cheap transmitter.
std::vector<Note> compose_boogie() {
    ScoreBuilder s;
    constexpr double kBeat = 60.0 / 132.0;
    constexpr std::array<int, 8> kBassLine{41, 45, 48, 51, 48, 45, 43, 45};
    for (int bar = 0; bar < 7; ++bar) {
        const double bt = 25.9 + bar * 4.0 * kBeat;
        for (int e = 0; e < 8; ++e) {
            s.note(bt + e * kBeat * 0.5, 0.20, kBassLine[static_cast<std::size_t>(e)], 0.6,
                   Voice::kBoogieBass);
        }
        for (const double off : {1.5, 3.5}) {
            for (const int m : {65, 69, 72}) {
                s.note(bt + off * kBeat, 0.16, m, 0.5, Voice::kBoogieLead);
            }
        }
    }
    return std::move(s.notes);
}

// Streams a note list: per frame, notes whose time has come become active
// voices, and finished ones are retired. All state lives per note, so the
// same class serves both the anthem and the jalopy's radio.
class MusicSynth {
   public:
    explicit MusicSynth(std::vector<Note> notes) : notes_(std::move(notes)) {
        std::ranges::sort(notes_, {}, &Note::t);
    }

    void render(double t0, std::span<double> out) {
        const double t_end = t0 + static_cast<double>(out.size()) * kDt;
        while (next_ < notes_.size() && notes_[next_].t < t_end) {
            Active a;
            a.note = notes_[next_];
            a.timbre.set_cutoff(voice_cutoff(a.note));
            switch (a.note.voice) {
                case Voice::kHorn:
                    a.thump.set_cutoff(900.0);  // breath noise band
                    break;
                case Voice::kTimpani:
                    a.thump.set_cutoff(200.0);  // mallet thump
                    break;
                case Voice::kPad:
                    a.thump.set_cutoff(800.0);  // bow-noise highpass helper
                    break;
                case Voice::kHarp: {
                    // Pluck a string: a Karplus-Strong loop the length of
                    // one period, excited with a noise burst whose
                    // brightness follows the pluck strength.
                    const auto len = static_cast<std::size_t>(
                        std::clamp(48000.0 / a.note.hz, 24.0, 2000.0));
                    a.ks.assign(len, 0.0f);
                    OnePole pick;
                    pick.set_cutoff(1200.0 + 6000.0 * a.note.vel);
                    double mean = 0.0;
                    for (auto& sample_out : a.ks) {
                        sample_out = static_cast<float>(pick.process(rng_.bipolar()));
                        mean += static_cast<double>(sample_out);
                    }
                    mean /= static_cast<double>(len);
                    for (auto& sample_out : a.ks) {
                        sample_out -= static_cast<float>(mean);
                    }
                    // Each cell sees ks_rho once per loop PASS (once per
                    // period), not once per sample - so the exponent scales
                    // by the loop length, same as the Hall's comb gains.
                    const double t60 = std::clamp(6.0 - a.note.hz / 300.0, 1.2, 5.0);
                    a.ks_rho = std::pow(
                        10.0, -3.0 * static_cast<double>(len) / (t60 * 48000.0));
                    break;
                }
                default:
                    a.thump.set_cutoff(300.0);
                    break;
            }
            ++next_;
            active_.push_back(a);
        }
        std::ranges::fill(out, 0.0);
        for (auto& a : active_) {
            for (std::size_t i = 0; i < out.size(); ++i) {
                const double tt = t0 + static_cast<double>(i) * kDt - a.note.t;
                if (tt < 0.0) {
                    continue;
                }
                out[i] += sample(a, tt);
                if (a.done) {
                    break;
                }
            }
        }
        std::erase_if(active_, [](const Active& a) { return a.done; });
    }

   private:
    struct Active {
        Note note;
        double phase = 0.0;
        double phase2 = 0.25;
        double phase3 = 0.5;
        double phase4 = 0.75;
        double phase5 = 0.1;
        OnePole timbre;
        OnePole thump;
        // Chamberlin state-variable filter: the resonant lowpass whose
        // cutoff rides the envelope - brass brightness follows loudness.
        double svf_lp = 0.0;
        double svf_bp = 0.0;
        std::array<double, 6> mphase{};  // timpani modes / cymbal partials
        std::array<double, 5> walk{};    // string-section detune drift
        std::vector<float> ks;           // harp string (Karplus-Strong)
        std::size_t ks_i = 0;
        double ks_rho = 1.0;
        bool done = false;
    };

    static double voice_cutoff(const Note& n) {
        switch (n.voice) {
            case Voice::kHorn:
                return std::min(2600.0, 4.5 * n.hz);
            case Voice::kTrumpet:
                return std::min(5200.0, 7.0 * n.hz);
            case Voice::kPad:
                return 1500.0;
            case Voice::kTimpani:
                return 700.0;
            case Voice::kHarp:
                return std::min(6000.0, 8.0 * n.hz);
            case Voice::kCymbal:
                return 9000.0;
            case Voice::kBoogieLead:
                return 3000.0;
            case Voice::kBoogieBass:
                return 900.0;
        }
        return 2000.0;
    }

    // Attack/decay/sustain while the note holds, exponential tail after.
    static double envelope(double tt, double dur, double a, double d, double sus, double r,
                           bool& done) {
        const double gate = std::min(tt, dur);
        double level = sus;
        if (gate < a) {
            level = gate / a;
        } else if (gate < a + d) {
            level = 1.0 - (1.0 - sus) * (gate - a) / d;
        }
        if (tt > dur) {
            const double tail = std::exp(-5.0 * (tt - dur) / r);
            if (tail < 0.004) {
                done = true;
                return 0.0;
            }
            level *= tail;
        }
        return level;
    }

    // Chamberlin state-variable lowpass with a hint of its bandpass mixed
    // in: resonance where the cutoff sits, which is what an opening brass
    // bore does and a one-pole cannot.
    static double svf(Active& a, double x, double fc_hz, double damp) {
        const double f = 2.0 * std::sin(kPi * std::min(fc_hz, 6500.0) * kDt);
        a.svf_lp += f * a.svf_bp;
        const double hp = x - a.svf_lp - damp * a.svf_bp;
        a.svf_bp += f * hp;
        return a.svf_lp + 0.22 * a.svf_bp;
    }

    double sample(Active& a, double tt) {
        const Note& n = a.note;
        switch (n.voice) {
            case Voice::kHorn: {
                const double env = envelope(tt, n.dur, 0.07, 0.22, 0.75, 0.4, a.done);
                // Scoop into the note from below, the way a hand-stopped
                // bell speaks; vibrato arrives late and shallow.
                const double scoop = 1.0 - 0.023 * std::exp(-tt / 0.06);
                const double vib =
                    1.0 + 0.0035 * std::clamp((tt - 0.4) / 0.5, 0.0, 1.0) *
                              std::sin(kTau * 4.3 * tt);
                const double inc = n.hz * scoop * vib * kDt;
                double raw =
                    0.5 * (blep_saw(a.phase, inc) + blep_saw(a.phase2, inc * 1.0013));
                raw += 0.06 * std::exp(-tt / 0.05) * a.thump.process(rng_.bipolar());
                // The brass cue that matters most: brightness follows
                // loudness. The bore opens as the envelope does.
                const double fc = std::min(n.hz * (1.3 + 4.2 * env * n.vel), 3200.0);
                const double out = svf(a, raw, fc, 0.85);
                return std::tanh(out * (1.4 + 2.2 * env * n.vel)) * env * n.vel * 0.32;
            }
            case Voice::kTrumpet: {
                const double env = envelope(tt, n.dur, 0.03, 0.12, 0.8, 0.28, a.done);
                const double over = 1.0 + 0.3 * std::exp(-tt / 0.045);  // attack blip
                const double scoop = 1.0 - 0.016 * std::exp(-tt / 0.03);
                const double vib =
                    1.0 + 0.0035 * std::clamp((tt - 0.35) / 0.4, 0.0, 1.0) *
                              std::sin(kTau * 5.6 * tt + 1.0);
                const double inc = n.hz * scoop * vib * kDt;
                const double saw = blep_saw(a.phase, inc);
                const double sq = saw - blep_saw(a.phase2, inc);
                const double raw = 0.6 * saw + 0.4 * sq;
                const double fc = std::min(n.hz * (1.8 + 11.0 * env * n.vel), 6200.0);
                const double out = svf(a, raw, fc, 0.8);
                return std::tanh(out * (1.2 + 2.8 * env * n.vel)) * env * over * n.vel *
                       0.26;
            }
            case Voice::kPad: {
                // A section, not an oscillator: five saws whose detunes
                // drift independently (nobody's bow is anybody else's),
                // with a little high bow-noise riding the envelope.
                const double attack = std::max(0.25, 0.9 - 0.6 * n.vel);
                const double env = envelope(tt, n.dur, attack, 0.5, 0.85, 1.2, a.done);
                constexpr std::array<double, 5> kDetune{-0.0070, -0.0032, 0.0, 0.0035,
                                                        0.0074};
                std::array<double*, 5> phases{&a.phase, &a.phase2, &a.phase3, &a.phase4,
                                              &a.phase5};
                double sum = 0.0;
                for (std::size_t k = 0; k < 5; ++k) {
                    a.walk[k] = std::clamp(a.walk[k] + rng_.bipolar() * kDt * 0.06,
                                           -0.002, 0.002);
                    const double inc = n.hz * (1.0 + kDetune[k] + a.walk[k]) * kDt;
                    sum += blep_saw(*phases[k], inc);
                }
                // Drawn into a local first: two rng_ calls in one expression
                // would leave the draw order compiler-defined, and the
                // rendered output must be bit-identical across toolchains.
                const double white = rng_.bipolar();
                const double bow = 0.018 * (white - a.thump.process(rng_.bipolar()));
                return (a.timbre.process(sum * 0.2) + bow * env) * env * n.vel * 0.30;
            }
            case Voice::kTimpani: {
                if (tt > 2.6) {
                    a.done = true;
                    return 0.0;
                }
                // A kettledrum is a set of inharmonic membrane modes, not a
                // sine: principal plus the 1.5 / 1.74 / 2.0 / 2.25 series
                // (air-loaded drumhead), higher modes dying faster, all of
                // it bending down slightly as the head settles.
                constexpr std::array<double, 5> kRatio{1.0, 1.504, 1.742, 2.0, 2.245};
                constexpr std::array<double, 5> kAmp{1.0, 0.62, 0.40, 0.28, 0.20};
                // Genuine T60s per mode (exp(-6.91 t / T60) is -60 dB at
                // T60), so by the 2.6 s cut the principal sits near -78 dB
                // and the stop is inaudible.
                constexpr std::array<double, 5> kModeT60{2.0, 1.3, 0.9, 0.65, 0.5};
                const double bend = 1.0 + 0.025 * std::exp(-tt / 0.08);
                double sum = 0.0;
                for (std::size_t k = 0; k < 5; ++k) {
                    a.mphase[k] += n.hz * kRatio[k] * bend * kDt;
                    sum += kAmp[k] * std::exp(-6.91 * tt / kModeT60[k]) *
                           std::sin(kTau * a.mphase[k]);
                }
                const double thump =
                    a.thump.process(rng_.bipolar()) * std::exp(-tt * 26.0) * 2.2;
                return (0.55 * sum + 0.5 * thump) * n.vel * 0.5;
            }
            case Voice::kHarp: {
                // The Karplus-Strong loop plucked at activation: read, damp
                // by averaging (the string's own high-frequency loss), feed
                // back with the tuned decay.
                if (a.ks.empty() || tt > 6.0) {
                    a.done = true;
                    return 0.0;
                }
                const std::size_t next_i = (a.ks_i + 1) % a.ks.size();
                const double out = static_cast<double>(a.ks[a.ks_i]);
                a.ks[a.ks_i] = static_cast<float>(
                    0.5 * (static_cast<double>(a.ks[a.ks_i]) +
                           static_cast<double>(a.ks[next_i])) *
                    a.ks_rho);
                a.ks_i = next_i;
                return out * n.vel * 0.55;
            }
            case Voice::kCymbal: {
                // A swell into a crash: band-limited noise for the wash,
                // plus ring-modulated inharmonic partial pairs for the
                // metal underneath it.
                const double rise = std::min(tt / std::max(n.dur, 0.05), 1.0);
                const double ring = tt > n.dur ? std::exp(-(tt - n.dur) / 1.4) : rise * rise;
                if (tt > n.dur + 4.0) {
                    a.done = true;
                    return 0.0;
                }
                constexpr std::array<double, 6> kPartial{923.7, 1369.9, 1780.2,
                                                         2230.1, 2782.5, 3484.3};
                for (std::size_t k = 0; k < 6; ++k) {
                    a.mphase[k] += kPartial[k] * kDt;
                }
                const double metal = std::sin(kTau * a.mphase[0]) * std::sin(kTau * a.mphase[1]) +
                                     std::sin(kTau * a.mphase[2]) * std::sin(kTau * a.mphase[3]) +
                                     std::sin(kTau * a.mphase[4]) * std::sin(kTau * a.mphase[5]);
                const double white = rng_.bipolar();  // sequenced, as in kPad
                const double sizzle =
                    a.timbre.process(white) - a.thump.process(rng_.bipolar());
                return (0.65 * sizzle + 0.28 * metal) * ring * n.vel * 0.32;
            }
            case Voice::kBoogieLead: {
                const double inc = n.hz * kDt;
                const double sq = blep_saw(a.phase, inc) - blep_saw(a.phase2, inc);
                return a.timbre.process(sq) *
                       envelope(tt, n.dur, 0.008, 0.08, 0.5, 0.1, a.done) * n.vel * 0.22;
            }
            case Voice::kBoogieBass: {
                const double inc = n.hz * kDt;
                return a.timbre.process(blep_saw(a.phase, inc)) *
                       envelope(tt, n.dur, 0.01, 0.1, 0.7, 0.12, a.done) * n.vel * 0.30;
            }
        }
        return 0.0;
    }

    std::vector<Note> notes_;
    std::size_t next_ = 0;
    std::vector<Active> active_;
    Rng rng_{0x71ADE5EEDull};
};

// ---------------------------------------------------------------------------
// The scoring stage. A small Schroeder reverb (four damped combs into two
// allpasses, ~1.9 s RT60) applied to the anthem before it ever reaches the
// transmitter: the hall is part of the recording the station is playing,
// which is half of what makes a synthesized orchestra read as an orchestra.
// ---------------------------------------------------------------------------

class Hall {
   public:
    Hall() {
        constexpr std::array<std::size_t, 4> kCombLen{1687, 1601, 2053, 2251};
        for (std::size_t k = 0; k < combs_.size(); ++k) {
            combs_[k].buf.assign(kCombLen[k], 0.0);
            combs_[k].gain =
                std::pow(10.0, -3.0 * static_cast<double>(kCombLen[k]) / (1.9 * 48000.0));
            combs_[k].damp.set_cutoff(4200.0);
        }
        allpasses_[0].buf.assign(389, 0.0);
        allpasses_[1].buf.assign(127, 0.0);
    }

    // Returns the wet signal only; the caller chooses the blend.
    double process(double x) {
        double s = 0.0;
        for (auto& c : combs_) {
            const double y = c.buf[c.i];
            c.buf[c.i] = x + c.damp.process(y) * c.gain;
            c.i = (c.i + 1) % c.buf.size();
            s += y;
        }
        s *= 0.25;
        for (auto& ap : allpasses_) {
            const double y = ap.buf[ap.i];
            const double out = y - 0.7 * s;
            ap.buf[ap.i] = s + 0.7 * out;
            ap.i = (ap.i + 1) % ap.buf.size();
            s = out;
        }
        return s;
    }

   private:
    struct Comb {
        std::vector<double> buf;
        std::size_t i = 0;
        double gain = 0.8;
        OnePole damp;
    };
    struct Allpass {
        std::vector<double> buf;
        std::size_t i = 0;
    };
    std::array<Comb, 4> combs_{};
    std::array<Allpass, 2> allpasses_{};
};

// ---------------------------------------------------------------------------
// The transmitter. Bandpass, drive, flutter, hiss, the odd crackle - and a
// mix knob, because at 0:38 the picture cuts close and the broadcast opens
// up from "radio somewhere out there" to "the PA is right in front of you".
// ---------------------------------------------------------------------------

class RadioFx {
   public:
    RadioFx(double hp_hz, double lp_hz, double flutter_hz, double flutter_depth, double hiss,
            double crackle_rate, std::uint64_t seed)
        : hp_(Biquad::highpass(hp_hz, 0.707)),
          lp_(Biquad::lowpass(lp_hz, 0.707)),
          flutter_hz_(flutter_hz),
          flutter_depth_(flutter_depth),
          hiss_(hiss),
          crackle_rate_(crackle_rate),
          rng_(seed) {}

    // amount 1 = full radio, 0 = untouched.
    double process(double x, double t, double amount) {
        // The drive is an upward compressor for quiet material (~+5 dB), so
        // its makeup gain is kept low - otherwise the "distant radio" ends
        // up louder per unit of object gain than the clean close-up mix and
        // the 0:38 cut lands backwards.
        const double banded = lp_.process(hp_.process(x));
        const double driven = std::tanh(2.4 * banded) * 0.55;
        const double flutter =
            1.0 - flutter_depth_ * (0.5 + 0.5 * std::sin(kTau * flutter_hz_ * t +
                                                         0.7 * std::sin(kTau * 0.13 * t)));
        if (rng_.next01() < crackle_rate_ * kDt) {
            crackle_ = 0.4 + 0.6 * rng_.next01();
        }
        crackle_ *= kCrackleDecay;
        const double grit = (hiss_ + 0.5 * crackle_) * rng_.bipolar();
        const double radio = driven * flutter + grit;
        return radio * amount + x * (1.0 - amount);
    }

   private:
    static inline const double kCrackleDecay = std::exp(-kDt * 160.0);
    Biquad hp_;
    Biquad lp_;
    double flutter_hz_;
    double flutter_depth_;
    double hiss_;
    double crackle_rate_;
    double crackle_ = 0.0;
    Rng rng_;
};

// ---------------------------------------------------------------------------
// Sound effects. Each is a stateful generator writing one frame of mono
// essence; macro-dynamics (approach, swamp, recede) live in the keyframe
// gains, so these only provide character.
// ---------------------------------------------------------------------------

// Icy shimmer, sparkle pings, and a soft whoosh underneath.
class Comet {
   public:
    Comet() {
        hi_.set_cutoff(6000.0);
        lo_.set_cutoff(2000.0);
        whoosh_.set_cutoff(260.0);
    }
    void render([[maybe_unused]] double t0, std::span<float> out) {
        for (std::size_t i = 0; i < out.size(); ++i) {
            const double white = rng_.bipolar();
            const double shimmer = hi_.process(white) - lo_.process(white);
            const double whoosh = whoosh_.process(rng2_.bipolar());
            if (rng_.next01() < 9.0 * kDt) {
                pings_[next_ping_] = {0.0, 2000.0 + 4000.0 * rng_.next01(),
                                      0.3 + 0.4 * rng_.next01()};
                next_ping_ = (next_ping_ + 1) % pings_.size();
            }
            double ping_sum = 0.0;
            for (auto& p : pings_) {
                if (p.amp <= 0.0) {
                    continue;
                }
                p.t += kDt;
                const double env = std::exp(-p.t / 0.06);
                if (env < 0.01) {
                    p.amp = 0.0;
                    continue;
                }
                ping_sum += p.amp * env * std::sin(kTau * p.hz * p.t);
            }
            out[i] += static_cast<float>(0.45 * shimmer + 0.8 * whoosh + 0.5 * ping_sum);
        }
    }

   private:
    struct Ping {
        double t = 0.0;
        double hz = 0.0;
        double amp = 0.0;
    };
    Rng rng_{0xC0DE7A11u};
    Rng rng2_{0x0FF1CE99u};
    OnePole hi_;
    OnePole lo_;
    OnePole whoosh_;
    std::array<Ping, 8> pings_{};
    std::size_t next_ping_ = 0;
};

// A tired freighter: low harmonic stack with a slow throb, plus LF noise.
class EngineRumble {
   public:
    explicit EngineRumble(double f0) : f0_(f0) { noise_lp_.set_cutoff(130.0); }
    void render(double t0, double dopp0, double dopp1, std::span<float> out) {
        constexpr std::array<double, 4> kRatio{1.0, 2.02, 3.05, 4.1};
        constexpr std::array<double, 4> kAmp{1.0, 0.6, 0.35, 0.2};
        for (std::size_t i = 0; i < out.size(); ++i) {
            const double t = t0 + static_cast<double>(i) * kDt;
            const double mix = static_cast<double>(i) / static_cast<double>(out.size());
            const double f0 = f0_ * (dopp0 + (dopp1 - dopp0) * mix);
            double tone = 0.0;
            for (std::size_t k = 0; k < kRatio.size(); ++k) {
                phases_[k] += f0 * kRatio[k] * kDt;
                tone += kAmp[k] * std::sin(kTau * phases_[k]);
            }
            const double throb = 1.0 + 0.22 * std::sin(kTau * 6.8 * t);
            const double noise = noise_lp_.process(rng_.bipolar());
            out[i] += static_cast<float>((0.28 * tone + 0.5 * noise) * throb);
        }
    }

   private:
    double f0_;
    std::array<double, 4> phases_{};
    OnePole noise_lp_;
    Rng rng_{0xD1E5E1u};
};

// Turbine whine plus a whistle, with an optional comms squawk window.
class RunaboutWhine {
   public:
    RunaboutWhine(double f0, double squawk_start, double squawk_end, std::uint64_t seed)
        : f0_(f0), squawk_start_(squawk_start), squawk_end_(squawk_end), rng_(seed) {
        air_.set_cutoff(1000.0);
        syllable_.set_cutoff(14.0);
        squawk_hp_ = Biquad::highpass(1200.0, 0.9);
        squawk_lp_ = Biquad::lowpass(2200.0, 0.9);
    }
    void render(double t0, double dopp0, double dopp1, std::span<float> out) {
        for (std::size_t i = 0; i < out.size(); ++i) {
            const double t = t0 + static_cast<double>(i) * kDt;
            const double mix = static_cast<double>(i) / static_cast<double>(out.size());
            const double dopp = dopp0 + (dopp1 - dopp0) * mix;
            const double f0 = f0_ * dopp;
            ph1_ += f0 * kDt;
            ph2_ += f0 * 2.51 * kDt;
            ph3_ += 2350.0 * dopp * (1.0 + 0.004 * std::sin(kTau * 6.0 * t)) * kDt;
            const double whine = 0.5 * std::sin(kTau * ph1_) + 0.25 * std::sin(kTau * ph2_);
            const double whistle = 0.18 * std::sin(kTau * ph3_);
            const double air = 0.06 * (rng_.bipolar() - air_.process(rng_.bipolar()));
            const double flutter = 1.0 + 0.15 * std::sin(kTau * 10.7 * t);
            double squawk = 0.0;
            if (t >= squawk_start_ && t < squawk_end_) {
                // Syllable-rate gating over band-limited noise reads as chatter
                // from a cockpit without saying anything at all.
                const double gate =
                    syllable_.process(rng_.next01() > 0.45 ? 1.0 : 0.1);
                squawk = squawk_lp_.process(squawk_hp_.process(rng_.bipolar())) * gate * 1.4;
            }
            out[i] += static_cast<float>((whine + whistle + air) * flutter + squawk);
        }
    }

   private:
    double f0_;
    double squawk_start_;
    double squawk_end_;
    double ph1_ = 0.0;
    double ph2_ = 0.33;
    double ph3_ = 0.66;
    OnePole air_;
    OnePole syllable_;
    Biquad squawk_hp_;
    Biquad squawk_lp_;
    Rng rng_;
};

// Welding: bursts of dense crackle over a faint arc hum.
class WeldPod {
   public:
    WeldPod() { spark_hp_ = Biquad::highpass(1800.0, 0.8); }
    void render([[maybe_unused]] double t0, std::span<float> out) {
        for (std::size_t i = 0; i < out.size(); ++i) {
            if (burst_ <= 0.0 && rng_.next01() < 1.7 * kDt) {
                burst_ = 0.15 + 0.35 * rng_.next01();
            }
            double spark = 0.0;
            if (burst_ > 0.0) {
                burst_ -= kDt;
                if (rng_.next01() < 400.0 * kDt) {
                    spark = 0.9 * rng_.bipolar();
                }
            }
            arc_ += 118.0 * kDt;
            const double hum = 0.08 * (std::sin(kTau * arc_) > 0.0 ? 1.0 : -1.0);
            out[i] += static_cast<float>(spark_hp_.process(spark) + hum);
        }
    }

   private:
    Biquad spark_hp_;
    double burst_ = 0.0;
    double arc_ = 0.0;
    Rng rng_{0x5EAB012u};
};

// The wormhole's core: a saturated subsonic swell that glides up as it
// opens and falls away as it collapses. Nearly all of it rides lfe_send.
class WormholeCore {
   public:
    WormholeCore() {
        sub_.set_cutoff(45.0);
        roar_.set_cutoff(300.0);
    }
    void render(double t0, std::span<float> out) {
        for (std::size_t i = 0; i < out.size(); ++i) {
            const double t = t0 + static_cast<double>(i) * kDt;
            const double rel = t - 103.0;
            if (rel < 0.0) {
                continue;
            }
            double f = 26.0 + 26.0 * std::clamp(rel / 5.0, 0.0, 1.0);
            if (t > 111.5) {
                f *= std::exp(-(t - 111.5) * 0.35);
            }
            phase_ += f * kDt;
            const double core = std::tanh(3.0 * std::sin(kTau * phase_)) * 0.9;
            const double sub = 0.4 * sub_.process(rng_.bipolar());
            const double roar =
                0.35 * roar_.process(rng_.bipolar()) * std::min(rel / 5.0, 1.0);
            out[i] += static_cast<float>(core + sub + roar);
        }
    }

   private:
    double phase_ = 0.0;
    OnePole sub_;
    OnePole roar_;
    Rng rng_{0xB16D001u};
};

// The bloom: inharmonic partials on slow random walks, each breathing at its
// own rate - the part of the wormhole that wraps up and over the room.
class WormholeShimmer {
   public:
    explicit WormholeShimmer(std::uint64_t seed) : rng_(seed) {
        air_.set_cutoff(3500.0);
        for (std::size_t k = 0; k < kBase.size(); ++k) {
            lfo_phase_[k] = rng_.next01();
        }
    }
    void render(double t0, std::span<float> out) {
        constexpr double kLfoHz = 0.21;
        for (std::size_t i = 0; i < out.size(); ++i) {
            const double t = t0 + static_cast<double>(i) * kDt;
            if (t < 103.5) {
                continue;
            }
            double sum = 0.0;
            for (std::size_t k = 0; k < kBase.size(); ++k) {
                walk_[k] = std::clamp(walk_[k] + rng_.bipolar() * kDt * 0.5, -0.04, 0.04);
                phase_[k] += kBase[k] * (1.0 + walk_[k]) * kDt;
                lfo_phase_[k] += kLfoHz * (1.0 + 0.3 * static_cast<double>(k)) * kDt;
                const double am = 0.5 + 0.5 * std::sin(kTau * lfo_phase_[k]);
                sum += am * std::sin(kTau * phase_[k]);
            }
            const double sparkle = 0.12 * (rng_.bipolar() - air_.process(rng_.bipolar()));
            out[i] += static_cast<float>(0.5 * sum / static_cast<double>(kBase.size()) * 2.0 +
                                         sparkle);
        }
    }

   private:
    static constexpr std::array<double, 5> kBase{287.0, 419.0, 563.0, 743.0, 1063.0};
    Rng rng_;
    OnePole air_;
    std::array<double, 5> phase_{};
    std::array<double, 5> walk_{};
    std::array<double, 5> lfo_phase_{};
};

// ---------------------------------------------------------------------------
// The scene: ten objects, each with an authored KeyframePath. Positions are
// room-anchored per §4.2.1 (x 0 left wall to 1 right, y 0 front to 1 back,
// z -1 floor to +1 ceiling); the listener sits at room centre, the station
// far front-centre. Distance lives in the authored gains.
// ---------------------------------------------------------------------------

enum Object : std::size_t {
    kBroadcast = 0,
    kComet,
    kJalopyEngine,
    kJalopyRadio,
    kRunaboutA,
    kRunaboutB,
    kWorkPod,
    kWormholeCore,
    kShimmerL,
    kShimmerR,
    kObjectCount,
};

ac3::oba::ObjectPath make_path(std::vector<ac3::oba::Keyframe> keyframes) {
    auto path = ac3::oba::KeyframePath::create(std::move(keyframes));
    if (!path) {
        std::fputs("internal error: bad keyframe table\n", stderr);
        std::exit(1);
    }
    return ac3::oba::ObjectPath{std::move(*path)};
}

std::vector<ac3::oba::ObjectPath> build_paths() {
    using ac3::oba::Keyframe;
    std::vector<ac3::oba::ObjectPath> paths;
    paths.reserve(kObjectCount);

    // kBroadcast: nailed to the station, far front-centre. Its gain arc IS
    // the edit: faint, firmer at the reveal, full after the close-up cut,
    // swelling for the reprise, easing out under the wormhole.
    paths.push_back(make_path({
        {0.0, {0.5, 0.04, 0.05}, 0.00, 0.0},
        {2.0, {0.5, 0.04, 0.05}, 0.05, 0.0},
        {14.0, {0.5, 0.04, 0.05}, 0.10, 0.0},
        {15.5, {0.5, 0.04, 0.05}, 0.30, 0.0},
        {37.5, {0.5, 0.05, 0.05}, 0.30, 0.0},
        {39.0, {0.5, 0.10, 0.08}, 0.62, 0.06},
        {50.0, {0.5, 0.10, 0.08}, 0.55, 0.05},
        {60.5, {0.5, 0.10, 0.08}, 0.58, 0.06},
        {80.0, {0.5, 0.10, 0.08}, 0.62, 0.08},
        {103.3, {0.5, 0.10, 0.08}, 0.58, 0.08},
        {104.0, {0.5, 0.10, 0.08}, 0.55, 0.10},
        {110.0, {0.5, 0.08, 0.06}, 0.50, 0.06},
        {113.0, {0.5, 0.06, 0.05}, 0.32, 0.02},
        {115.0, {0.5, 0.04, 0.05}, 0.00, 0.0},
    }));

    // kComet: left to right across the front, closest mid-screen.
    paths.push_back(make_path({
        {0.0, {0.02, 0.30, 0.15}, 0.00, 0.0},
        {1.5, {0.08, 0.28, 0.15}, 0.35, 0.0},
        {6.5, {0.45, 0.22, 0.20}, 0.50, 0.03},
        {12.0, {0.88, 0.30, 0.15}, 0.30, 0.0},
        {14.0, {0.98, 0.35, 0.10}, 0.00, 0.0},
    }));

    // kJalopyEngine: in from the rear right, right past the camera, off
    // toward the station shrinking to a dot.
    paths.push_back(make_path({
        {25.5, {0.92, 0.95, 0.05}, 0.00, 0.0},
        {28.0, {0.85, 0.75, 0.08}, 0.35, 0.10},
        {31.0, {0.78, 0.45, 0.12}, 0.65, 0.30},
        {33.5, {0.55, 0.18, 0.15}, 0.70, 0.35},
        {35.5, {0.48, 0.10, 0.10}, 0.35, 0.10},
        {38.0, {0.46, 0.05, 0.05}, 0.12, 0.0},
        {40.0, {0.46, 0.04, 0.05}, 0.00, 0.0},
    }));

    // kJalopyRadio: same flight, its own gain - at closest approach it is
    // louder than the station (0.75 vs 0.30) and simply drowns it.
    paths.push_back(make_path({
        {25.5, {0.92, 0.95, 0.05}, 0.00, 0.0},
        {28.0, {0.85, 0.75, 0.08}, 0.30, 0.0},
        {31.0, {0.78, 0.45, 0.12}, 0.60, 0.0},
        {33.5, {0.55, 0.18, 0.15}, 0.75, 0.0},
        {35.5, {0.48, 0.10, 0.10}, 0.25, 0.0},
        {38.0, {0.46, 0.05, 0.05}, 0.08, 0.0},
        {40.0, {0.46, 0.04, 0.05}, 0.00, 0.0},
    }));

    // kRunaboutA: undocks front-centre, arcs right and OVERHEAD (z 0.65 -
    // real height metadata for a renderer with tops), exits over the rear.
    paths.push_back(make_path({
        {51.5, {0.52, 0.08, 0.05}, 0.00, 0.0},
        {53.0, {0.58, 0.15, 0.10}, 0.25, 0.05},
        {56.0, {0.78, 0.35, 0.30}, 0.50, 0.12},
        {58.5, {0.60, 0.55, 0.65}, 0.65, 0.18},
        {61.0, {0.40, 0.80, 0.35}, 0.40, 0.08},
        {64.0, {0.30, 0.95, 0.15}, 0.15, 0.0},
        {66.5, {0.28, 0.98, 0.10}, 0.00, 0.0},
    }));

    // kRunaboutB: the port-side pass, rear-left to a front-left docking.
    paths.push_back(make_path({
        {71.5, {0.08, 0.92, 0.10}, 0.00, 0.0},
        {74.0, {0.12, 0.70, 0.20}, 0.35, 0.08},
        {77.0, {0.18, 0.42, 0.30}, 0.55, 0.14},
        {79.5, {0.30, 0.20, 0.18}, 0.35, 0.06},
        {82.0, {0.42, 0.08, 0.08}, 0.12, 0.0},
        {84.0, {0.44, 0.06, 0.05}, 0.00, 0.0},
    }));

    // kWorkPod: parked against an upper pylon, front-right, slightly raised.
    paths.push_back(make_path({
        {83.5, {0.62, 0.10, 0.35}, 0.00, 0.0},
        {85.0, {0.62, 0.10, 0.35}, 0.28, 0.0},
        {89.0, {0.62, 0.10, 0.35}, 0.25, 0.0},
        {90.5, {0.62, 0.10, 0.35}, 0.00, 0.0},
    }));

    // kWormholeCore: beyond the station. Mostly LFE - lfe_send is the only
    // route to that channel (ac3/oba/atmos.hpp).
    paths.push_back(make_path({
        {102.5, {0.5, 0.02, 0.10}, 0.00, 0.0},
        {105.0, {0.5, 0.02, 0.10}, 0.38, 0.50},
        {108.0, {0.5, 0.05, 0.15}, 0.45, 0.80},
        {111.5, {0.5, 0.05, 0.15}, 0.42, 0.70},
        {113.5, {0.5, 0.02, 0.05}, 0.15, 0.15},
        {114.8, {0.5, 0.02, 0.05}, 0.00, 0.0},
    }));

    // kShimmerL / kShimmerR: the bloom splits in two and wraps outward,
    // upward, and back - the pair is what makes the opening feel bigger
    // than the front wall.
    paths.push_back(make_path({
        {104.0, {0.50, 0.03, 0.20}, 0.00, 0.0},
        {106.0, {0.35, 0.15, 0.45}, 0.30, 0.0},
        {108.5, {0.18, 0.35, 0.75}, 0.45, 0.05},
        {111.5, {0.25, 0.25, 0.60}, 0.35, 0.0},
        {113.5, {0.45, 0.06, 0.20}, 0.08, 0.0},
        {114.6, {0.50, 0.03, 0.10}, 0.00, 0.0},
    }));
    paths.push_back(make_path({
        {104.0, {0.50, 0.03, 0.20}, 0.00, 0.0},
        {106.0, {0.65, 0.15, 0.45}, 0.30, 0.0},
        {108.5, {0.82, 0.35, 0.75}, 0.45, 0.05},
        {111.5, {0.75, 0.25, 0.60}, 0.35, 0.0},
        {113.5, {0.55, 0.06, 0.20}, 0.08, 0.0},
        {114.6, {0.50, 0.03, 0.10}, 0.00, 0.0},
    }));

    return paths;
}

// Radial velocity toward the listener (room centre), for Doppler. The room
// is nominally 30 m square and 8 m tall - cinematic, not architectural.
double listener_distance_m(const ac3::oba::Position& p) {
    const double dx = (p.x - 0.5) * 30.0;
    const double dy = (p.y - 0.5) * 30.0;
    const double dz = p.z * 8.0;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

struct Cue {
    double t;
    const char* label;
};

constexpr std::array<Cue, 11> kCues{{
    {0.0, "a comet drifts across the black, left to right"},
    {2.0, "somewhere ahead, a station is broadcasting its anthem"},
    {14.0, "the station pans into view - tinny, distant"},
    {26.0, "cargo jalopy flypast: engine rumble and someone's boogie radio"},
    {38.0, "cut to close-up: the broadcast opens to full bandwidth"},
    {52.0, "runabout undocks, sweeps overhead, squawks the tower"},
    {61.0, "the anthem surges back for the reprise"},
    {72.0, "second runabout crosses the port side"},
    {84.0, "maintenance pod welding on the upper pylon"},
    {103.0, "the wormhole opens behind the station"},
    {111.5, "...and swallows itself"},
}};

bool write_bytes(const std::string& path, std::span<const std::byte> bytes) {
    std::ofstream out{path, std::ios::binary};
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        std::printf("error: cannot write %s\n", path.c_str());
        return false;
    }
    return true;
}

// User-supplied broadcast material: mixed to mono, linearly resampled to
// 48 kHz, peak-normalized. Linear interpolation is beneath the codec but
// fine for a source that then goes through a radio.
std::vector<double> load_music(const std::string& path) {
    const auto wav = ac3::io::read_wav(path);
    if (!wav) {
        std::printf("error: %s: %s\n", path.c_str(),
                    std::string{ac3::io::describe(wav.error())}.c_str());
        return {};
    }
    const std::size_t frames = wav->frame_count();
    if (frames == 0) {
        std::printf("error: %s: empty file\n", path.c_str());
        return {};
    }
    std::vector<double> mono(frames, 0.0);
    for (const auto& channel : wav->channels) {
        for (std::size_t i = 0; i < frames; ++i) {
            mono[i] += static_cast<double>(channel[i]);
        }
    }
    const double ratio = static_cast<double>(wav->sample_rate) / kRate;
    const std::size_t out_frames =
        static_cast<std::size_t>(static_cast<double>(frames) / ratio);
    std::vector<double> resampled(out_frames, 0.0);
    double peak = 1e-9;
    for (std::size_t i = 0; i < out_frames; ++i) {
        const double pos = static_cast<double>(i) * ratio;
        const auto i0 = static_cast<std::size_t>(pos);
        const std::size_t i1 = std::min(i0 + 1, frames - 1);
        const double frac = pos - static_cast<double>(i0);
        resampled[i] = mono[i0] + (mono[i1] - mono[i0]) * frac;
        peak = std::max(peak, std::abs(resampled[i]));
    }
    const double norm = 0.8 / peak;
    for (auto& s : resampled) {
        s *= norm;
    }
    return resampled;
}

}  // namespace

int main(int argc, char** argv) {
    const bool smoke_test = argc < 2;
    const std::string prefix = smoke_test ? std::string{} : std::string{argv[1]};
    double seconds = 115.0;
    if (argc > 2) {
        seconds = std::clamp(std::atof(argv[2]), 4.0, 115.0);
    }
    std::vector<double> user_music;
    if (argc > 3) {
        user_music = load_music(argv[3]);
        if (user_music.empty()) {
            return 1;
        }
    }
    // The smoke test renders the busiest stretch - reveal into jalopy
    // flypast - and writes nothing, so ctest stays side-effect free.
    const double render_until = smoke_test ? 34.0 : seconds;
    const double encode_from = smoke_test ? 24.0 : 0.0;

    // Object metadata competes with the mantissas for the same frame; ten
    // objects want more headroom than atmos_objects.cpp's three.
    constexpr std::uint32_t kBitrateKbps = 640;
    ac3::oba::AtmosEncoder objects_encoder{{.bitrate_kbps = kBitrateKbps},
                                           static_cast<int>(kObjectCount)};
    // Same scene with the EMDF container omitted: the fallback for decoders
    // that validate emdf_protection (see ac3/oba/atmos.hpp for why this is
    // objects-or-nothing, never both).
    ac3::oba::AtmosEncoder bed51_encoder{
        {.bitrate_kbps = kBitrateKbps, .emit_object_metadata = false},
        static_cast<int>(kObjectCount)};

    const auto paths = build_paths();
    MusicSynth anthem{compose_cover()};
    MusicSynth boogie{compose_boogie()};
    Hall hall;
    RadioFx station_radio{300.0, 3100.0, 0.7, 0.12, 0.010, 1.2, 0xA11CE5u};
    RadioFx jalopy_radio{500.0, 2400.0, 1.1, 0.22, 0.018, 3.0, 0xBEEFCAFEu};
    Comet comet;
    EngineRumble jalopy_engine{52.0};
    RunaboutWhine runabout_a{175.0, 58.2, 60.0, 0x0A57EE1u};
    RunaboutWhine runabout_b{195.0, -1.0, -1.0, 0x0B57EE2u};
    WeldPod pod;
    WormholeCore wormhole;
    WormholeShimmer shimmer_l{0x117E57u};
    WormholeShimmer shimmer_r{0x227E58u};

    std::vector<std::vector<float>> essences(kObjectCount,
                                             std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> views(kObjectCount);
    std::array<double, ac3::kSamplesPerFrame> music_scratch{};

    const auto total_frames = static_cast<std::uint64_t>(
        (render_until * kRate + (ac3::kSamplesPerFrame - 1)) / ac3::kSamplesPerFrame);
    const auto first_encoded_frame = static_cast<std::uint64_t>(
        encode_from * kRate / ac3::kSamplesPerFrame);

    std::vector<std::byte> objects_stream;
    std::vector<std::byte> bed51_stream;
    std::vector<std::vector<float>> bed_out(6);
    if (!smoke_test) {
        const auto samples =
            static_cast<std::size_t>(total_frames) * ac3::kSamplesPerFrame;
        for (auto& channel : bed_out) {
            channel.reserve(samples);
        }
    }

    // Doppler state: previous distance per tracked object, plus this frame's
    // and last frame's factor so the pitch ramps smoothly across a frame
    // instead of stepping.
    std::array<double, kObjectCount> prev_dist{};
    std::array<double, kObjectCount> dopp{};
    std::array<double, kObjectCount> prev_dopp{};
    dopp.fill(1.0);
    prev_dopp.fill(1.0);
    bool doppler_primed = false;

    std::size_t next_cue = 0;
    std::uint64_t n0 = 0;
    for (std::uint64_t frame = 0; frame < total_frames; ++frame) {
        const double t_start = static_cast<double>(n0) / kRate;
        const double t_end =
            static_cast<double>(n0 + ac3::kSamplesPerFrame) / kRate;
        while (next_cue < kCues.size() && kCues[next_cue].t <= t_start) {
            const double t = kCues[next_cue].t;
            std::printf("[%d:%04.1f] %s\n", static_cast<int>(t / 60.0),
                        std::fmod(t, 60.0), kCues[next_cue].label);
            ++next_cue;
        }

        // Both metadata layers interpolate to the END of the frame, so
        // placements are evaluated there - same convention as ac3cli.
        const auto placement = ac3::oba::evaluate_placements(paths, t_end);

        for (const std::size_t obj :
             {kJalopyEngine, kRunaboutA, kRunaboutB}) {
            const double dist = listener_distance_m(placement[obj].position);
            double factor = 1.0;
            if (doppler_primed) {
                const double v =
                    (dist - prev_dist[obj]) / (ac3::kSamplesPerFrame / kRate);
                factor = std::clamp(343.0 / (343.0 + v), 0.86, 1.16);
            }
            prev_dist[obj] = dist;
            prev_dopp[obj] = dopp[obj];
            dopp[obj] = factor;
        }
        doppler_primed = true;

        for (auto& essence : essences) {
            std::ranges::fill(essence, 0.0f);
        }

        // The broadcast: the anthem (or your recording) through the station
        // transmitter. Full radio until the close-up cut at 0:38, opening to
        // nearly clean over 1.5 s; a floor of 0.15 keeps a little PA colour.
        anthem.render(t_start, music_scratch);
        for (int n = 0; n < ac3::kSamplesPerFrame; ++n) {
            const auto i = static_cast<std::size_t>(n);
            const double t = t_start + static_cast<double>(n) * kDt;
            double src = music_scratch[i];
            if (!user_music.empty()) {
                // A supplied recording brings its own ambience; the hall is
                // only for the built-in synth orchestra.
                const double pos = (t - 2.0) * kRate;
                src = 0.0;
                if (pos >= 0.0 &&
                    static_cast<std::size_t>(pos) < user_music.size()) {
                    src = user_music[static_cast<std::size_t>(pos)];
                }
            } else {
                src += 0.32 * hall.process(src);
            }
            const double amount =
                t < 38.0 ? 1.0 : std::max(0.15, 1.0 - (t - 38.0) / 1.5 * 0.85);
            // The station's PA has a limiter: the full orchestra at the
            // finale would otherwise stack past unity against the wormhole.
            essences[kBroadcast][i] = static_cast<float>(
                std::tanh(1.15 * station_radio.process(src, t, amount)) * 0.92);
        }

        // The jalopy's radio never opens up; it stays cheap and fluttery.
        if (t_end > 25.0 && t_start < 41.0) {
            boogie.render(t_start, music_scratch);
            for (int n = 0; n < ac3::kSamplesPerFrame; ++n) {
                const auto i = static_cast<std::size_t>(n);
                const double t = t_start + static_cast<double>(n) * kDt;
                essences[kJalopyRadio][i] = static_cast<float>(
                    jalopy_radio.process(music_scratch[i], t, 1.0));
            }
            jalopy_engine.render(t_start, prev_dopp[kJalopyEngine],
                                 dopp[kJalopyEngine], essences[kJalopyEngine]);
        }
        if (t_end > 0.0 && t_start < 15.0) {
            comet.render(t_start, essences[kComet]);
        }
        if (t_end > 51.0 && t_start < 67.0) {
            runabout_a.render(t_start, prev_dopp[kRunaboutA], dopp[kRunaboutA],
                              essences[kRunaboutA]);
        }
        if (t_end > 71.0 && t_start < 85.0) {
            runabout_b.render(t_start, prev_dopp[kRunaboutB], dopp[kRunaboutB],
                              essences[kRunaboutB]);
        }
        if (t_end > 83.0 && t_start < 91.0) {
            pod.render(t_start, essences[kWorkPod]);
        }
        if (t_end > 102.0) {
            wormhole.render(t_start, essences[kWormholeCore]);
            shimmer_l.render(t_start, essences[kShimmerL]);
            shimmer_r.render(t_start, essences[kShimmerR]);
        }

        n0 += ac3::kSamplesPerFrame;
        if (frame < first_encoded_frame) {
            continue;
        }
        for (std::size_t obj = 0; obj < kObjectCount; ++obj) {
            views[obj] = essences[obj];
        }

        const auto unit = objects_encoder.encode_frame(views, placement);
        if (!unit) {
            std::printf("atmos encode failed at %.1f s: %d\n", t_start,
                        std::to_underlying(unit.error()));
            return 1;
        }
        objects_stream.insert(objects_stream.end(), unit->bytes.begin(),
                              unit->bytes.end());
        if (!smoke_test) {
            const auto bed_unit = bed51_encoder.encode_frame(views, placement);
            if (!bed_unit) {
                std::printf("bed51 encode failed at %.1f s: %d\n", t_start,
                            std::to_underlying(bed_unit.error()));
                return 1;
            }
            bed51_stream.insert(bed51_stream.end(), bed_unit->bytes.begin(),
                                bed_unit->bytes.end());
            const auto bed = objects_encoder.bed();
            for (std::size_t ch = 0; ch < bed_out.size(); ++ch) {
                bed_out[ch].insert(bed_out[ch].end(), bed[ch].begin(),
                                   bed[ch].end());
            }
        }
    }

    const auto encoded_frames = total_frames - first_encoded_frame;
    std::printf("%llu access units, %zu bytes of DD+ with %d objects over a 5.1 bed\n",
                static_cast<unsigned long long>(encoded_frames),
                objects_stream.size(), objects_encoder.dynamic_object_count());
    if (smoke_test) {
        return 0;
    }

    if (!write_bytes(prefix + ".ec3", objects_stream) ||
        !write_bytes(prefix + "_bed51.ec3", bed51_stream)) {
        return 1;
    }

    // The bed a legacy decoder hears, as a WAV in FL FR FC LFE BL BR order.
    const auto order = ac3::io::wav_channel_order(ac3::Acmod::k3_2, true);
    if (!ac3::io::write_wav_f32(prefix + "_bed.wav", bed_out, 48000, order)) {
        std::printf("error: cannot write %s_bed.wav\n", prefix.c_str());
        return 1;
    }

    // Lo/Ro headphone fold-down of that bed (L C R Ls Rs LFE in, stereo out).
    const std::size_t samples = bed_out[0].size();
    std::vector<std::vector<float>> stereo(2, std::vector<float>(samples));
    double peak = 1e-9;
    for (std::size_t i = 0; i < samples; ++i) {
        const double c = 0.707 * static_cast<double>(bed_out[1][i]);
        const double lfe = 0.35 * static_cast<double>(bed_out[5][i]);
        const double lo = static_cast<double>(bed_out[0][i]) + c +
                          0.707 * static_cast<double>(bed_out[3][i]) + lfe;
        const double ro = static_cast<double>(bed_out[2][i]) + c +
                          0.707 * static_cast<double>(bed_out[4][i]) + lfe;
        stereo[0][i] = static_cast<float>(lo);
        stereo[1][i] = static_cast<float>(ro);
        peak = std::max({peak, std::abs(lo), std::abs(ro)});
    }
    const auto norm = static_cast<float>(0.95 / std::max(peak, 0.95));
    for (auto& channel : stereo) {
        for (auto& s : channel) {
            s *= norm;
        }
    }
    if (!ac3::io::write_wav_f32(prefix + "_stereo.wav", stereo, 48000)) {
        std::printf("error: cannot write %s_stereo.wav\n", prefix.c_str());
        return 1;
    }

    std::printf("wrote %s.ec3 (objects), %s_bed51.ec3 (no container), "
                "%s_bed.wav, %s_stereo.wav\n",
                prefix.c_str(), prefix.c_str(), prefix.c_str(), prefix.c_str());
    return 0;
}
