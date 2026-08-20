// pybind11 bindings for ac3forge (roadmap F2) - wraps ac3::FrameEncoder, ac3::FrameDecoder,
// ac3::Eac3Decoder and ac3::oba::AtmosEncoder directly (pybind11-direct, per the roadmap's own
// dependency note - no intermediate C API). Every C++ class kept here is exactly the one
// declared in src/lib/include/ac3/{encoder/encoder,decoder/decoder,oba/atmos}.hpp; this file adds
// no codec behaviour of its own; error handling exists only because Python has no
// std::expected-shaped calling convention.
//
// GIL handling: argument conversion (numpy -> owned std::vector, bytes -> owned
// std::vector<std::byte>) happens with the GIL held, since it touches Python objects; the actual
// encode/decode call runs inside a py::gil_scoped_release block operating only on C++ locals, and
// results are converted back to Python objects after that block ends (GIL reacquired by the
// guard's destructor, including on the exception path - unwinding through the block still runs
// it). This is deliberately explicit per call site rather than py::call_guard, which only wraps
// the C++ invocation and not this file's chosen "own the buffer, don't hold a live view into a
// Python object across the release" pattern.

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cstring>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/meta/drc.hpp"
#include "ac3/meta/mixing.hpp"
#include "ac3/oba/atmos.hpp"
#include "ac3/oba/oamd.hpp"

namespace py = pybind11;

namespace {

// --- kwargs-constructible plain config structs ------------------------------
// Every EncoderConfig/AtmosConfig/DecoderConfig/Profile/HeavyConfig/Position/ObjectPlacement
// field already has a real C++ default, so a Python caller only needs to name what they want to
// change - this is the one piece of binding-layer machinery that gets reused across all of them,
// in place of writing the same "collect kwargs, apply known ones, reject the rest" logic six
// times over. Rejecting unknown keys (rather than silently ignoring them) catches the ordinary
// typo ("EncoderConfig(dialnrm=10)") that a silently-accepted **kwargs would hide as a
// wrong-but-legal default.
template <typename T>
class KwargBinder {
   public:
    explicit KwargBinder(py::kwargs kwargs) : kwargs_(std::move(kwargs)) {}

    template <typename V>
    KwargBinder& field(const char* name, V T::*member) {
        if (kwargs_.contains(name)) {
            // kwargs_[name] is an access through the implicit this-> of a class template, so
            // two-phase lookup treats it as dependent regardless of kwargs_'s own (non-dependent)
            // declared type - Clang enforces the standard's `template` disambiguator here and
            // rejects the call without it; MSVC/GCC merely tolerate the omission as a
            // permissive extension. Confirmed the hard way: this built clean on both of those
            // and only failed in CI's real AppleClang leg.
            value_.*member = kwargs_[name].template cast<V>();
            seen_.insert(name);
        }
        return *this;
    }

    T finish() {
        for (auto item : kwargs_) {
            const auto key = py::cast<std::string>(item.first);
            if (!seen_.contains(key)) {
                throw py::type_error("unexpected keyword argument '" + key + "'");
            }
        }
        return value_;
    }

   private:
    py::kwargs kwargs_;
    T value_{};
    std::set<std::string> seen_;
};

// --- error translation -------------------------------------------------------
// FrameError has no describe() on the C++ side (see docs/library/index.md's own conventions
// list) - the message here is the enumerator's own name, not an invented description.
std::string frame_error_name(ac3::FrameError e) {
    switch (e) {
        case ac3::FrameError::kInvalidBitrate: return "kInvalidBitrate";
        case ac3::FrameError::kInvalidDialnorm: return "kInvalidDialnorm";
        case ac3::FrameError::kInvalidSubstream: return "kInvalidSubstream";
        case ac3::FrameError::kInvalidChannelMap: return "kInvalidChannelMap";
        case ac3::FrameError::kTooManyChannels: return "kTooManyChannels";
        case ac3::FrameError::kInvalidMixLevel: return "kInvalidMixLevel";
        case ac3::FrameError::kInvalidObjectAudio: return "kInvalidObjectAudio";
    }
    return "unknown";
}

struct EncodeFailure : std::runtime_error {
    ac3::FrameError code;
    explicit EncodeFailure(ac3::FrameError c)
        : std::runtime_error("ac3forge encode failed: FrameError." + frame_error_name(c)), code(c) {}
};

struct DecodeFailure : std::runtime_error {
    ac3::DecodeError code;
    explicit DecodeFailure(ac3::DecodeError c)
        : std::runtime_error("ac3forge decode failed: " + std::string(ac3::describe(c))), code(c) {}
};

// --- buffer / array plumbing -------------------------------------------------

std::vector<std::byte> to_bytes(const py::buffer& buf) {
    const py::buffer_info info = buf.request();
    if (info.ndim != 1) {
        throw py::value_error("expected a 1-D bytes-like object");
    }
    const auto total = static_cast<std::size_t>(info.size) * static_cast<std::size_t>(info.itemsize);
    std::vector<std::byte> out(total);
    if (total > 0) {
        std::memcpy(out.data(), info.ptr, total);
    }
    return out;
}

// channels: a Python sequence of 1-D array-likes, one per audio channel, AC-3 order (Table 5.8,
// LFE last - see docs/library/index.md's own "Channel order" convention). expected_len == 0
// skips the length check (used nowhere today, kept for the one call site that legitimately has
// no fixed length).
std::vector<std::vector<float>> extract_channels(const py::sequence& channels,
                                                  std::size_t expected_len) {
    std::vector<std::vector<float>> out;
    out.reserve(static_cast<std::size_t>(py::len(channels)));
    for (const auto& item : channels) {
        py::array_t<float, py::array::c_style | py::array::forcecast> arr(item);
        if (arr.ndim() != 1) {
            throw py::value_error("each channel must be a 1-D array");
        }
        const auto n = static_cast<std::size_t>(arr.shape(0));
        if (expected_len != 0 && n != expected_len) {
            throw py::value_error("each channel must have exactly " + std::to_string(expected_len) +
                                   " samples (ac3.SAMPLES_PER_FRAME)");
        }
        const float* data = arr.data();
        out.emplace_back(data, data + n);
    }
    return out;
}

std::vector<std::span<const float>> as_spans(const std::vector<std::vector<float>>& owned) {
    std::vector<std::span<const float>> spans;
    spans.reserve(owned.size());
    for (const auto& channel : owned) {
        spans.emplace_back(channel);
    }
    return spans;
}

py::array_t<float> to_ndarray(const std::vector<float>& v) {
    py::array_t<float> arr(static_cast<py::ssize_t>(v.size()));
    if (!v.empty()) {
        std::memcpy(arr.mutable_data(), v.data(), v.size() * sizeof(float));
    }
    return arr;
}

py::list channels_to_list(const std::vector<std::vector<float>>& channels) {
    py::list out;
    for (const auto& channel : channels) {
        out.append(to_ndarray(channel));
    }
    return out;
}

std::vector<std::uint8_t> to_vec(const std::array<std::uint8_t, ac3::kBlocksPerFrame>& a) {
    return {a.begin(), a.end()};
}

py::list blksw_to_list(const std::vector<std::array<bool, ac3::kBlocksPerFrame>>& blksw) {
    py::list out;
    for (const auto& per_channel : blksw) {
        py::list inner;
        for (bool b : per_channel) {
            inner.append(b);
        }
        out.append(inner);
    }
    return out;
}

// AC-3's own channel labels (Table 5.8) - independent of the E-AC-3 Table E2.5 chanmap machinery
// used for DecodedSubstream/DecodedAccessUnit below, same split apps/wasm/decoder_bindings.cpp
// draws between its own ac3_channel_labels() and apply_layout()/apply_layout_substream().
std::vector<std::string> ac3_channel_labels(ac3::Acmod acmod, bool lfe) {
    static const std::array<std::vector<std::string>, 8> kByAcmod{{
        {"Ch1", "Ch2"},              // kDualMono
        {"C"},                       // k1_0
        {"L", "R"},                  // k2_0
        {"L", "C", "R"},             // k3_0
        {"L", "R", "S"},             // k2_1
        {"L", "C", "R", "S"},        // k3_1
        {"L", "R", "Ls", "Rs"},      // k2_2
        {"L", "C", "R", "Ls", "Rs"}  // k3_2
    }};
    auto labels = kByAcmod[static_cast<std::uint8_t>(acmod)];
    if (lfe) {
        labels.emplace_back("LFE");
    }
    return labels;
}

std::vector<std::string> chanmap_labels(std::uint16_t map) {
    std::vector<std::string> labels;
    const auto layout = ac3::eac3::chanmap::expand(map);
    for (int i = 0; i < layout.count; ++i) {
        labels.emplace_back(ac3::eac3::chanmap::name(layout[i]));
    }
    return labels;
}

}  // namespace

PYBIND11_MODULE(_ac3forge, m) {
    m.doc() = "pybind11 bindings for ac3forge - AC-3/E-AC-3 encode/decode plus Atmos objects";

    m.attr("SAMPLES_PER_FRAME") = ac3::kSamplesPerFrame;
    m.attr("BLOCKS_PER_FRAME") = ac3::kBlocksPerFrame;

    // --- exceptions ----------------------------------------------------------
    // Defined here (rather than pure-Python subclasses of RuntimeError) so the extension owns
    // its own exception identity - ac3forge/__init__.py just re-exports these three names.
    static py::exception<std::runtime_error> ac3_error(m, "Ac3Error", PyExc_RuntimeError);
    static py::exception<std::runtime_error> encode_error(m, "Ac3EncodeError", ac3_error.ptr());
    static py::exception<std::runtime_error> decode_error(m, "Ac3DecodeError", ac3_error.ptr());

    // Constructing through PyObject_CallFunction rather than py::exception<T>'s own operator()
    // (deprecated, and only ever sets a plain string message) - this needs a real INSTANCE so
    // ".error" can be attached to it before it becomes the active exception.
    py::register_exception_translator([](std::exception_ptr p) {
        if (!p) {
            return;
        }
        try {
            std::rethrow_exception(p);
        } catch (const EncodeFailure& e) {
            py::object exc =
                py::reinterpret_steal<py::object>(PyObject_CallFunction(encode_error.ptr(), "s", e.what()));
            exc.attr("error") = py::cast(e.code);
            PyErr_SetObject(encode_error.ptr(), exc.ptr());
        } catch (const DecodeFailure& e) {
            py::object exc =
                py::reinterpret_steal<py::object>(PyObject_CallFunction(decode_error.ptr(), "s", e.what()));
            exc.attr("error") = py::cast(e.code);
            PyErr_SetObject(decode_error.ptr(), exc.ptr());
        }
    });

    // --- enums -----------------------------------------------------------------
    py::enum_<ac3::Acmod>(m, "Acmod", "A/52 Table 5.8 audio coding mode")
        .value("kDualMono", ac3::Acmod::kDualMono, "1+1: two independent programmes (Ch1, Ch2)")
        .value("k1_0", ac3::Acmod::k1_0, "C")
        .value("k2_0", ac3::Acmod::k2_0, "L, R")
        .value("k3_0", ac3::Acmod::k3_0, "L, C, R")
        .value("k2_1", ac3::Acmod::k2_1, "L, R, S")
        .value("k3_1", ac3::Acmod::k3_1, "L, C, R, S")
        .value("k2_2", ac3::Acmod::k2_2, "L, R, Ls, Rs")
        .value("k3_2", ac3::Acmod::k3_2, "L, C, R, Ls, Rs");

    py::enum_<ac3::SampleRate>(m, "SampleRate")
        .value("k48000", ac3::SampleRate::k48000)
        .value("k44100", ac3::SampleRate::k44100)
        .value("k32000", ac3::SampleRate::k32000)
        .value("k24000", ac3::SampleRate::k24000, "E-AC-3 fscod2 only")
        .value("k22050", ac3::SampleRate::k22050, "E-AC-3 fscod2 only")
        .value("k16000", ac3::SampleRate::k16000, "E-AC-3 fscod2 only");

    py::enum_<ac3::DecodeError>(m, "DecodeError")
        .value("kTruncated", ac3::DecodeError::kTruncated)
        .value("kBadSyncWord", ac3::DecodeError::kBadSyncWord)
        .value("kBadCrc", ac3::DecodeError::kBadCrc)
        .value("kReservedValue", ac3::DecodeError::kReservedValue)
        .value("kUnsupported", ac3::DecodeError::kUnsupported)
        .value("kInvalidStream", ac3::DecodeError::kInvalidStream);

    py::enum_<ac3::FrameError>(m, "FrameError")
        .value("kInvalidBitrate", ac3::FrameError::kInvalidBitrate)
        .value("kInvalidDialnorm", ac3::FrameError::kInvalidDialnorm)
        .value("kInvalidSubstream", ac3::FrameError::kInvalidSubstream)
        .value("kInvalidChannelMap", ac3::FrameError::kInvalidChannelMap)
        .value("kTooManyChannels", ac3::FrameError::kTooManyChannels)
        .value("kInvalidMixLevel", ac3::FrameError::kInvalidMixLevel)
        .value("kInvalidObjectAudio", ac3::FrameError::kInvalidObjectAudio);

    py::enum_<ac3::eac3::StreamType>(m, "StreamType")
        .value("kIndependent", ac3::eac3::StreamType::kIndependent)
        .value("kDependent", ac3::eac3::StreamType::kDependent)
        .value("kConvertible", ac3::eac3::StreamType::kConvertible);

    py::enum_<ac3::meta::ProfileId>(m, "ProfileId", "Conventional Dolby DRC profiles (ac3.profile_for)")
        .value("kFilmStandard", ac3::meta::ProfileId::kFilmStandard)
        .value("kFilmLight", ac3::meta::ProfileId::kFilmLight)
        .value("kMusicStandard", ac3::meta::ProfileId::kMusicStandard)
        .value("kMusicLight", ac3::meta::ProfileId::kMusicLight)
        .value("kSpeech", ac3::meta::ProfileId::kSpeech);

    py::enum_<ac3::meta::CentreMixLevel>(m, "CentreMixLevel")
        .value("kMinus3dB", ac3::meta::CentreMixLevel::kMinus3dB)
        .value("kMinus4_5dB", ac3::meta::CentreMixLevel::kMinus4_5dB)
        .value("kMinus6dB", ac3::meta::CentreMixLevel::kMinus6dB);

    py::enum_<ac3::meta::SurroundMixLevel>(m, "SurroundMixLevel")
        .value("kMinus3dB", ac3::meta::SurroundMixLevel::kMinus3dB)
        .value("kMinus6dB", ac3::meta::SurroundMixLevel::kMinus6dB)
        .value("kSilent", ac3::meta::SurroundMixLevel::kSilent);

    // --- free functions ----------------------------------------------------
    m.def("fullbw_channel_count", &ac3::fullbw_channel_count, py::arg("acmod"));
    m.def("sample_rate_hz", &ac3::sample_rate_hz, py::arg("sample_rate"));
    m.def("describe", &ac3::describe, py::arg("error"), "Text for a DecodeError value");
    m.def(
        "profile_for", [](ac3::meta::ProfileId id) { return ac3::meta::profile(id); }, py::arg("id"),
        "The ac3.Profile for one of the conventional ac3.ProfileId presets");
    m.def(
        "profile_name", [](ac3::meta::ProfileId id) { return std::string(ac3::meta::profile_name(id)); },
        py::arg("id"));

    m.def(
        "split_frames",
        [](const py::buffer& stream) {
            const auto bytes = to_bytes(stream);
            const auto result = ac3::split_frames(bytes);
            if (!result) {
                throw DecodeFailure(result.error());
            }
            py::list out;
            for (const auto frame : *result) {
                out.append(py::bytes(reinterpret_cast<const char*>(frame.data()), frame.size()));
            }
            return out;
        },
        py::arg("stream"), "Split a raw AC-3/E-AC-3 elementary stream into individual syncframes");

    m.def(
        "split_access_units",
        [](const py::buffer& stream) {
            const auto bytes = to_bytes(stream);
            const auto result = ac3::split_access_units(bytes);
            if (!result) {
                throw DecodeFailure(result.error());
            }
            py::list out;
            for (const auto unit : *result) {
                out.append(py::bytes(reinterpret_cast<const char*>(unit.data()), unit.size()));
            }
            return out;
        },
        py::arg("stream"),
        "Group an E-AC-3 elementary stream's syncframes into access units (Eac3Decoder.decode_access_unit's own input shape)");

    m.def(
        "stream_bsid",
        [](const py::buffer& frame) {
            const auto bytes = to_bytes(frame);
            const auto result = ac3::stream_bsid(bytes);
            if (!result) {
                throw DecodeFailure(result.error());
            }
            return *result;
        },
        py::arg("frame"));

    // --- plain data types (kwargs-constructible where a caller builds one) -------------------
    py::class_<ac3::meta::Profile>(m, "Profile", "A/52 §7.7 DRC compression characteristic")
        .def(py::init([](py::kwargs kwargs) {
            return KwargBinder<ac3::meta::Profile>(std::move(kwargs))
                .field("null_low_db", &ac3::meta::Profile::null_low_db)
                .field("null_high_db", &ac3::meta::Profile::null_high_db)
                .field("boost_ratio", &ac3::meta::Profile::boost_ratio)
                .field("max_boost_db", &ac3::meta::Profile::max_boost_db)
                .field("early_cut_ratio", &ac3::meta::Profile::early_cut_ratio)
                .field("early_cut_end_db", &ac3::meta::Profile::early_cut_end_db)
                .field("cut_ratio", &ac3::meta::Profile::cut_ratio)
                .field("attack_ms", &ac3::meta::Profile::attack_ms)
                .field("release_ms", &ac3::meta::Profile::release_ms)
                .finish();
        }))
        .def_readwrite("null_low_db", &ac3::meta::Profile::null_low_db)
        .def_readwrite("null_high_db", &ac3::meta::Profile::null_high_db)
        .def_readwrite("boost_ratio", &ac3::meta::Profile::boost_ratio)
        .def_readwrite("max_boost_db", &ac3::meta::Profile::max_boost_db)
        .def_readwrite("early_cut_ratio", &ac3::meta::Profile::early_cut_ratio)
        .def_readwrite("early_cut_end_db", &ac3::meta::Profile::early_cut_end_db)
        .def_readwrite("cut_ratio", &ac3::meta::Profile::cut_ratio)
        .def_readwrite("attack_ms", &ac3::meta::Profile::attack_ms)
        .def_readwrite("release_ms", &ac3::meta::Profile::release_ms);

    py::class_<ac3::meta::HeavyConfig>(m, "HeavyConfig", "A/52 §7.7.2 heavy compression (RF mode)")
        .def(py::init([](py::kwargs kwargs) {
            return KwargBinder<ac3::meta::HeavyConfig>(std::move(kwargs))
                .field("dialogue_target_dbfs", &ac3::meta::HeavyConfig::dialogue_target_dbfs)
                .field("peak_ceiling_dbfs", &ac3::meta::HeavyConfig::peak_ceiling_dbfs)
                .field("release_db_per_second", &ac3::meta::HeavyConfig::release_db_per_second)
                .finish();
        }))
        .def_readwrite("dialogue_target_dbfs", &ac3::meta::HeavyConfig::dialogue_target_dbfs)
        .def_readwrite("peak_ceiling_dbfs", &ac3::meta::HeavyConfig::peak_ceiling_dbfs)
        .def_readwrite("release_db_per_second", &ac3::meta::HeavyConfig::release_db_per_second);

    py::class_<ac3::oba::Position>(m, "Position", "Room-anchored object position (§4.2.1)")
        .def(py::init([](py::kwargs kwargs) {
            return KwargBinder<ac3::oba::Position>(std::move(kwargs))
                .field("x", &ac3::oba::Position::x)
                .field("y", &ac3::oba::Position::y)
                .field("z", &ac3::oba::Position::z)
                .finish();
        }))
        .def_readwrite("x", &ac3::oba::Position::x)
        .def_readwrite("y", &ac3::oba::Position::y)
        .def_readwrite("z", &ac3::oba::Position::z)
        .def("__repr__", [](const ac3::oba::Position& p) {
            return "Position(x=" + std::to_string(p.x) + ", y=" + std::to_string(p.y) +
                   ", z=" + std::to_string(p.z) + ")";
        });

    py::class_<ac3::oba::ObjectPlacement>(m, "ObjectPlacement", "One object's placement for one frame")
        .def(py::init([](py::kwargs kwargs) {
            return KwargBinder<ac3::oba::ObjectPlacement>(std::move(kwargs))
                .field("position", &ac3::oba::ObjectPlacement::position)
                .field("gain", &ac3::oba::ObjectPlacement::gain)
                .field("lfe_send", &ac3::oba::ObjectPlacement::lfe_send)
                .finish();
        }))
        .def_readwrite("position", &ac3::oba::ObjectPlacement::position)
        .def_readwrite("gain", &ac3::oba::ObjectPlacement::gain)
        .def_readwrite("lfe_send", &ac3::oba::ObjectPlacement::lfe_send);

    py::class_<ac3::oba::DynamicObject>(m, "DynamicObject", "A decoded object's position and gain")
        .def_readonly("position", &ac3::oba::DynamicObject::position)
        .def_readonly("gain_db", &ac3::oba::DynamicObject::gain_db);

    py::class_<ac3::oba::Program>(m, "Program", "OAMD programme shape (§5.6)")
        .def_readonly("dynamic_only", &ac3::oba::Program::dynamic_only)
        .def_readonly("lfe", &ac3::oba::Program::lfe)
        .def_readonly("bed", &ac3::oba::Program::bed)
        .def_readonly("dynamic_objects", &ac3::oba::Program::dynamic_objects);

    py::class_<ac3::oba::DecodedProgram>(m, "DecodedProgram", "Decoded OAMD: programme shape plus every object")
        .def_readonly("program", &ac3::oba::DecodedProgram::program)
        .def_readonly("objects", &ac3::oba::DecodedProgram::objects);

    // --- encoder -------------------------------------------------------------
    py::class_<ac3::EncoderConfig>(m, "EncoderConfig")
        .def(py::init([](py::kwargs kwargs) {
            return KwargBinder<ac3::EncoderConfig>(std::move(kwargs))
                .field("sample_rate", &ac3::EncoderConfig::sample_rate)
                .field("bitrate_kbps", &ac3::EncoderConfig::bitrate_kbps)
                .field("dialnorm", &ac3::EncoderConfig::dialnorm)
                .field("dialnorm2", &ac3::EncoderConfig::dialnorm2)
                .field("chbwcod", &ac3::EncoderConfig::chbwcod)
                .field("acmod", &ac3::EncoderConfig::acmod)
                .field("lfe", &ac3::EncoderConfig::lfe)
                .field("coupling", &ac3::EncoderConfig::coupling)
                .field("cplbegf", &ac3::EncoderConfig::cplbegf)
                .field("cplendf", &ac3::EncoderConfig::cplendf)
                .field("fast_mdct", &ac3::EncoderConfig::fast_mdct)
                .field("drc", &ac3::EncoderConfig::drc)
                .field("heavy", &ac3::EncoderConfig::heavy)
                .field("drc2", &ac3::EncoderConfig::drc2)
                .field("heavy2", &ac3::EncoderConfig::heavy2)
                .field("cmixlev", &ac3::EncoderConfig::cmixlev)
                .field("surmixlev", &ac3::EncoderConfig::surmixlev)
                .finish();
        }))
        .def_readwrite("sample_rate", &ac3::EncoderConfig::sample_rate)
        .def_readwrite("bitrate_kbps", &ac3::EncoderConfig::bitrate_kbps)
        .def_readwrite("dialnorm", &ac3::EncoderConfig::dialnorm)
        .def_readwrite("dialnorm2", &ac3::EncoderConfig::dialnorm2)
        .def_readwrite("chbwcod", &ac3::EncoderConfig::chbwcod)
        .def_readwrite("acmod", &ac3::EncoderConfig::acmod)
        .def_readwrite("lfe", &ac3::EncoderConfig::lfe)
        .def_readwrite("coupling", &ac3::EncoderConfig::coupling)
        .def_readwrite("cplbegf", &ac3::EncoderConfig::cplbegf)
        .def_readwrite("cplendf", &ac3::EncoderConfig::cplendf)
        .def_readwrite("fast_mdct", &ac3::EncoderConfig::fast_mdct)
        .def_readwrite("drc", &ac3::EncoderConfig::drc)
        .def_readwrite("heavy", &ac3::EncoderConfig::heavy)
        .def_readwrite("drc2", &ac3::EncoderConfig::drc2)
        .def_readwrite("heavy2", &ac3::EncoderConfig::heavy2)
        .def_readwrite("cmixlev", &ac3::EncoderConfig::cmixlev)
        .def_readwrite("surmixlev", &ac3::EncoderConfig::surmixlev);

    py::class_<ac3::FrameEncoder>(m, "FrameEncoder")
        .def(py::init<const ac3::EncoderConfig&>(), py::arg("config"))
        .def(
            "encode_frame",
            [](ac3::FrameEncoder& self, const py::sequence& channels) {
                auto owned = extract_channels(channels, static_cast<std::size_t>(ac3::kSamplesPerFrame));
                std::vector<std::byte> bytes;
                {
                    py::gil_scoped_release release;
                    auto spans = as_spans(owned);
                    auto result = self.encode_frame(spans);
                    if (!result) {
                        throw EncodeFailure(result.error());
                    }
                    bytes = std::move(*result);
                }
                return py::bytes(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            },
            py::arg("channels"),
            "channels: a sequence of 1-D float arrays, ac3.SAMPLES_PER_FRAME samples each, AC-3 "
            "channel order (LFE last). Returns one syncframe as bytes.")
        .def_property_readonly("config", &ac3::FrameEncoder::config)
        .def_property_readonly("channel_count", &ac3::FrameEncoder::channel_count);

    // --- decoder ---------------------------------------------------------------
    py::class_<ac3::DecoderConfig>(m, "DecoderConfig")
        .def(py::init([](py::kwargs kwargs) {
            return KwargBinder<ac3::DecoderConfig>(std::move(kwargs))
                .field("drc_scale", &ac3::DecoderConfig::drc_scale)
                .field("heavy_compression", &ac3::DecoderConfig::heavy_compression)
                .finish();
        }))
        .def_readwrite("drc_scale", &ac3::DecoderConfig::drc_scale)
        .def_readwrite("heavy_compression", &ac3::DecoderConfig::heavy_compression);

    py::class_<ac3::DecodedFrame>(m, "DecodedFrame")
        .def_readonly("sample_rate", &ac3::DecodedFrame::sample_rate)
        .def_readonly("bitrate_kbps", &ac3::DecodedFrame::bitrate_kbps)
        .def_readonly("acmod", &ac3::DecodedFrame::acmod)
        .def_readonly("lfe", &ac3::DecodedFrame::lfe)
        .def_readonly("dialnorm", &ac3::DecodedFrame::dialnorm)
        .def_readonly("compr", &ac3::DecodedFrame::compr)
        .def_readonly("dialnorm2", &ac3::DecodedFrame::dialnorm2)
        .def_readonly("compr2", &ac3::DecodedFrame::compr2)
        .def_property_readonly("dynrng", [](const ac3::DecodedFrame& f) { return to_vec(f.dynrng); })
        .def_property_readonly("dynrng2", [](const ac3::DecodedFrame& f) { return to_vec(f.dynrng2); })
        .def_property_readonly("blksw", [](const ac3::DecodedFrame& f) { return blksw_to_list(f.blksw); })
        .def_property_readonly("channels",
                                [](const ac3::DecodedFrame& f) { return channels_to_list(f.channels); })
        .def_property_readonly("channel_labels", [](const ac3::DecodedFrame& f) {
            return ac3_channel_labels(f.acmod, f.lfe);
        });

    py::class_<ac3::DecodedSubstream>(m, "DecodedSubstream")
        .def_readonly("strmtyp", &ac3::DecodedSubstream::strmtyp)
        .def_readonly("substreamid", &ac3::DecodedSubstream::substreamid)
        .def_readonly("sample_rate", &ac3::DecodedSubstream::sample_rate)
        .def_readonly("acmod", &ac3::DecodedSubstream::acmod)
        .def_readonly("lfe", &ac3::DecodedSubstream::lfe)
        .def_readonly("dialnorm", &ac3::DecodedSubstream::dialnorm)
        .def_readonly("compr", &ac3::DecodedSubstream::compr)
        .def_readonly("dialnorm2", &ac3::DecodedSubstream::dialnorm2)
        .def_readonly("compr2", &ac3::DecodedSubstream::compr2)
        .def_readonly("numblkscod", &ac3::DecodedSubstream::numblkscod)
        .def_readonly("chanmap", &ac3::DecodedSubstream::chanmap)
        .def_readonly("last_dependent", &ac3::DecodedSubstream::last_dependent)
        .def_readonly("object_metadata", &ac3::DecodedSubstream::object_metadata)
        .def_property_readonly("dynrng", [](const ac3::DecodedSubstream& s) { return to_vec(s.dynrng); })
        .def_property_readonly("dynrng2", [](const ac3::DecodedSubstream& s) { return to_vec(s.dynrng2); })
        .def_property_readonly("blksw",
                                [](const ac3::DecodedSubstream& s) { return blksw_to_list(s.blksw); })
        .def_property_readonly("channels",
                                [](const ac3::DecodedSubstream& s) { return channels_to_list(s.channels); })
        .def_property_readonly(
            "object_audio", [](const ac3::DecodedSubstream& s) { return channels_to_list(s.object_audio); })
        .def_property_readonly("channel_labels", [](const ac3::DecodedSubstream& s) {
            return chanmap_labels(s.location_map());
        });

    py::class_<ac3::DecodedAccessUnit>(m, "DecodedAccessUnit")
        .def_readonly("sample_rate", &ac3::DecodedAccessUnit::sample_rate)
        .def_readonly("acmod", &ac3::DecodedAccessUnit::acmod)
        .def_readonly("dialnorm", &ac3::DecodedAccessUnit::dialnorm)
        .def_readonly("compr", &ac3::DecodedAccessUnit::compr)
        .def_readonly("numblkscod", &ac3::DecodedAccessUnit::numblkscod)
        .def_readonly("object_metadata", &ac3::DecodedAccessUnit::object_metadata)
        .def_readonly("substream_count", &ac3::DecodedAccessUnit::substream_count)
        .def_property_readonly("dynrng",
                                [](const ac3::DecodedAccessUnit& u) { return to_vec(u.dynrng); })
        .def_property_readonly(
            "object_audio", [](const ac3::DecodedAccessUnit& u) { return channels_to_list(u.object_audio); })
        .def_property_readonly("channels",
                                [](const ac3::DecodedAccessUnit& u) { return channels_to_list(u.channels); })
        .def_property_readonly("channel_labels", [](const ac3::DecodedAccessUnit& u) {
            // Dual mono has no Table E2.5 layout at all (DecodedAccessUnit::layout's own
            // comment) - fall back to the plain AC-3 acmod labels, same as
            // apps/wasm/decoder_bindings.cpp's apply_layout() does.
            if (u.layout.count == 0) {
                return ac3_channel_labels(u.acmod, false);
            }
            std::vector<std::string> labels;
            for (int i = 0; i < u.layout.count; ++i) {
                labels.emplace_back(ac3::eac3::chanmap::name(u.layout[i]));
            }
            return labels;
        });

    py::class_<ac3::FrameDecoder>(m, "FrameDecoder")
        .def(py::init<>())
        .def(py::init<const ac3::DecoderConfig&>(), py::arg("config"))
        .def(
            "decode_frame",
            [](ac3::FrameDecoder& self, const py::buffer& frame) {
                const auto bytes = to_bytes(frame);
                ac3::DecodedFrame result;
                {
                    py::gil_scoped_release release;
                    auto decoded = self.decode_frame(bytes);
                    if (!decoded) {
                        throw DecodeFailure(decoded.error());
                    }
                    result = std::move(*decoded);
                }
                return result;
            },
            py::arg("frame"), "Decode exactly one AC-3 syncframe");

    py::class_<ac3::Eac3Decoder>(m, "Eac3Decoder")
        .def(py::init<>())
        .def(py::init<const ac3::DecoderConfig&>(), py::arg("config"))
        .def(
            "decode_substream",
            [](ac3::Eac3Decoder& self, const py::buffer& frame) -> py::object {
                const auto bytes = to_bytes(frame);
                std::optional<ac3::DecodedSubstream> result;
                {
                    py::gil_scoped_release release;
                    auto decoded = self.decode_substream(bytes);
                    if (!decoded) {
                        throw DecodeFailure(decoded.error());
                    }
                    result = std::move(*decoded);
                }
                return result ? py::cast(*result) : py::none();
            },
            py::arg("frame"),
            "Decode one E-AC-3 syncframe. None means the frame's PCM is held back pending "
            "transient pre-noise processing (§3.7) - see flush().")
        .def(
            "decode_access_unit",
            [](ac3::Eac3Decoder& self, const py::buffer& unit) -> py::object {
                const auto bytes = to_bytes(unit);
                std::optional<ac3::DecodedAccessUnit> result;
                {
                    py::gil_scoped_release release;
                    auto decoded = self.decode_access_unit(bytes);
                    if (!decoded) {
                        throw DecodeFailure(decoded.error());
                    }
                    result = std::move(*decoded);
                }
                return result ? py::cast(*result) : py::none();
            },
            py::arg("unit"),
            "Decode one access unit (independent substream + dependents, as split_access_units "
            "delimits them). Same None convention as decode_substream, for the same reason.")
        .def("flush", [](ac3::Eac3Decoder& self) {
            std::vector<ac3::DecodedSubstream> out;
            {
                py::gil_scoped_release release;
                out = self.flush();
            }
            return out;
        });

    // --- Atmos objects -----------------------------------------------------
    py::class_<ac3::oba::AtmosConfig>(m, "AtmosConfig")
        .def(py::init([](py::kwargs kwargs) {
            return KwargBinder<ac3::oba::AtmosConfig>(std::move(kwargs))
                .field("sample_rate", &ac3::oba::AtmosConfig::sample_rate)
                .field("bitrate_kbps", &ac3::oba::AtmosConfig::bitrate_kbps)
                .field("dialnorm", &ac3::oba::AtmosConfig::dialnorm)
                .field("num_bands_idx", &ac3::oba::AtmosConfig::num_bands_idx)
                .field("fine_quant", &ac3::oba::AtmosConfig::fine_quant)
                .field("emit_object_metadata", &ac3::oba::AtmosConfig::emit_object_metadata)
                .field("fast_mdct", &ac3::oba::AtmosConfig::fast_mdct)
                .finish();
        }))
        .def_readwrite("sample_rate", &ac3::oba::AtmosConfig::sample_rate)
        .def_readwrite("bitrate_kbps", &ac3::oba::AtmosConfig::bitrate_kbps)
        .def_readwrite("dialnorm", &ac3::oba::AtmosConfig::dialnorm)
        .def_readwrite("num_bands_idx", &ac3::oba::AtmosConfig::num_bands_idx)
        .def_readwrite("fine_quant", &ac3::oba::AtmosConfig::fine_quant)
        .def_readwrite("emit_object_metadata", &ac3::oba::AtmosConfig::emit_object_metadata)
        .def_readwrite("fast_mdct", &ac3::oba::AtmosConfig::fast_mdct);

    py::class_<ac3::oba::AtmosEncoder>(m, "AtmosEncoder")
        .def(py::init<const ac3::oba::AtmosConfig&, int>(), py::arg("config"), py::arg("objects"))
        .def(
            "encode_frame",
            [](ac3::oba::AtmosEncoder& self, const py::sequence& objects,
               std::vector<ac3::oba::ObjectPlacement> placement) {
                auto owned =
                    extract_channels(objects, static_cast<std::size_t>(ac3::kSamplesPerFrame));
                std::vector<std::byte> bytes;
                {
                    py::gil_scoped_release release;
                    auto spans = as_spans(owned);
                    auto result = self.encode_frame(spans, placement);
                    if (!result) {
                        throw EncodeFailure(result.error());
                    }
                    bytes = std::move(result->bytes);
                }
                return py::bytes(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            },
            py::arg("objects"), py::arg("placement"),
            "objects: one mono ac3.SAMPLES_PER_FRAME array per object, in construction order. "
            "placement: one ac3.ObjectPlacement per object. Returns one E-AC-3 access unit as bytes.")
        .def_property_readonly("dynamic_object_count", &ac3::oba::AtmosEncoder::dynamic_object_count)
        .def_property_readonly("program", &ac3::oba::AtmosEncoder::program);
}
