#include "ac3/verify/mirror.hpp"

#include <algorithm>

namespace ac3::verify {

namespace {

// A stream's name in A/52's terms rather than by its internal index. The
// numbering is the encoder's and the decoder's shared one - fbw channels,
// then the LFE, then the coupling channel - so both sides' traces index
// identically and a name can be derived from the frame's shape alone.
std::string stream_name(int stream, int fbw_channels, int coded_channels) {
    if (stream < 0) {
        return {};
    }
    if (stream < fbw_channels) {
        return "channel " + std::to_string(stream);
    }
    if (stream < coded_channels) {
        return "LFE";
    }
    return "coupling";
}

// Appends at most kMaxPerArray mismatches between two arrays of the same
// length, so one desynced allocation cannot bury the block and stream
// identity under a few hundred identical-looking lines.
void diff_array(std::vector<Mismatch>& out, std::span<const std::uint8_t> encoder,
                std::span<const std::uint8_t> decoder, Field field, std::uint64_t frame,
                int block, int stream) {
    int reported = 0;
    for (std::size_t i = 0; i < encoder.size() && reported < kMaxPerArray; ++i) {
        if (encoder[i] == decoder[i]) {
            continue;
        }
        out.push_back({.frame = frame,
                       .block = block,
                       .stream = stream,
                       .index = static_cast<int>(i),
                       .field = field,
                       .encoder = encoder[i],
                       .decoder = decoder[i]});
        ++reported;
    }
}

void diff_delta(std::vector<Mismatch>& out, const DeltaSegments& encoder,
                const DeltaSegments& decoder, std::uint64_t frame, int block, int stream) {
    const auto push = [&](Field field, int index, long long e, long long d) {
        out.push_back({.frame = frame,
                       .block = block,
                       .stream = stream,
                       .index = index,
                       .field = field,
                       .encoder = e,
                       .decoder = d});
    };
    if (encoder.deltnseg != decoder.deltnseg) {
        push(Field::kDeltaSegmentCount, -1, encoder.deltnseg, decoder.deltnseg);
        // The per-segment comparison below would be comparing entries one side
        // never filled, so stop here: the counts differing IS the finding.
        return;
    }
    for (int seg = 0; seg < encoder.deltnseg; ++seg) {
        const auto i = static_cast<std::size_t>(seg);
        if (encoder.deltoffst[i] != decoder.deltoffst[i]) {
            push(Field::kDeltaOffset, seg, encoder.deltoffst[i], decoder.deltoffst[i]);
        }
        if (encoder.deltlen[i] != decoder.deltlen[i]) {
            push(Field::kDeltaLength, seg, encoder.deltlen[i], decoder.deltlen[i]);
        }
        if (encoder.deltba[i] != decoder.deltba[i]) {
            push(Field::kDeltaValue, seg, encoder.deltba[i], decoder.deltba[i]);
        }
    }
}

}  // namespace

void FrameTrace::reset() {
    fbw_channels = 0;
    coded_channels = 0;
    for (auto& block : blocks) {
        block.entered = false;
        block.allocated = false;
        block.bit_offset = 0;
        block.deltbaie = false;
        // clear(), not a fresh vector: a caller encoding a whole file reuses
        // one trace per frame, and the per-stream exponent/bap vectors are
        // the only allocation this facility makes at all.
        block.streams.clear();
    }
}

std::string_view describe(Field field) {
    switch (field) {
        case Field::kBlockReached:
            return "block reached";
        case Field::kBitOffset:
            return "bit offset at block start";
        case Field::kStreamCount:
            return "coded stream count";
        case Field::kDeltbaie:
            return "deltbaie";
        case Field::kDeltaSegmentCount:
            return "deltnseg";
        case Field::kDeltaOffset:
            return "deltoffst";
        case Field::kDeltaLength:
            return "deltlen";
        case Field::kDeltaValue:
            return "deltba";
        case Field::kAllocationReached:
            return "bit allocation computed";
        case Field::kExponentCount:
            return "exponent count";
        case Field::kExponent:
            return "exponent";
        case Field::kBapCount:
            return "bap count";
        case Field::kBap:
            return "bap";
    }
    return "unknown field";
}

std::string describe(const Mismatch& mismatch, int fbw_channels, int coded_channels) {
    std::string out = "frame " + std::to_string(mismatch.frame);
    if (mismatch.block >= 0) {
        out += " block " + std::to_string(mismatch.block);
    }
    if (mismatch.stream >= 0) {
        out += " " + stream_name(mismatch.stream, fbw_channels, coded_channels);
    }
    out += ": ";
    out += describe(mismatch.field);
    if (mismatch.index >= 0) {
        out += "[" + std::to_string(mismatch.index) + "]";
    }
    out += " encoder=" + std::to_string(mismatch.encoder);
    out += " decoder=" + std::to_string(mismatch.decoder);
    return out;
}

std::vector<Mismatch> compare(const FrameTrace& encoder, const FrameTrace& decoder,
                              std::uint64_t frame_index) {
    std::vector<Mismatch> out;
    const auto push = [&](int block, int stream, int index, Field field, long long e,
                          long long d) {
        out.push_back({.frame = frame_index,
                       .block = block,
                       .stream = stream,
                       .index = index,
                       .field = field,
                       .encoder = e,
                       .decoder = d});
    };

    for (int block = 0; block < kBlocksPerFrame; ++block) {
        const auto& e = encoder.blocks[static_cast<std::size_t>(block)];
        const auto& d = decoder.blocks[static_cast<std::size_t>(block)];
        if (!e.entered && !d.entered) {
            break;  // both sides are done; nothing beyond here was coded
        }
        if (e.entered != d.entered) {
            push(block, -1, -1, Field::kBlockReached, e.entered ? 1 : 0, d.entered ? 1 : 0);
            break;
        }

        // The localiser first: any disagreement about ANY field's width, from
        // any cause, lands here at the next block boundary.
        if (e.bit_offset != d.bit_offset) {
            push(block, -1, -1, Field::kBitOffset, static_cast<long long>(e.bit_offset),
                 static_cast<long long>(d.bit_offset));
        }
        if (e.deltbaie != d.deltbaie) {
            push(block, -1, -1, Field::kDeltbaie, e.deltbaie ? 1 : 0, d.deltbaie ? 1 : 0);
        }
        if (e.allocated != d.allocated) {
            push(block, -1, -1, Field::kAllocationReached, e.allocated ? 1 : 0,
                 d.allocated ? 1 : 0);
        } else if (e.allocated) {
            if (e.streams.size() != d.streams.size()) {
                push(block, -1, -1, Field::kStreamCount,
                     static_cast<long long>(e.streams.size()),
                     static_cast<long long>(d.streams.size()));
            } else {
                for (std::size_t s = 0; s < e.streams.size(); ++s) {
                    const auto& es = e.streams[s];
                    const auto& ds = d.streams[s];
                    const auto stream = static_cast<int>(s);
                    // Delta before exponents and bap: it is upstream of the
                    // allocation, so where it is the cause the other two are
                    // consequences, and a reader should meet the cause first.
                    diff_delta(out, es.delta, ds.delta, frame_index, block, stream);
                    if (es.exponents.size() != ds.exponents.size()) {
                        push(block, stream, -1, Field::kExponentCount,
                             static_cast<long long>(es.exponents.size()),
                             static_cast<long long>(ds.exponents.size()));
                    } else {
                        diff_array(out, es.exponents, ds.exponents, Field::kExponent,
                                   frame_index, block, stream);
                    }
                    if (es.bap.size() != ds.bap.size()) {
                        push(block, stream, -1, Field::kBapCount,
                             static_cast<long long>(es.bap.size()),
                             static_cast<long long>(ds.bap.size()));
                    } else {
                        diff_array(out, es.bap, ds.bap, Field::kBap, frame_index, block,
                                   stream);
                    }
                }
            }
        }

        if (!out.empty()) {
            // Everything after the first divergent block is a consequence of
            // it - see the header's note on why that is not reported.
            break;
        }
    }
    return out;
}

std::string report(std::span<const Mismatch> mismatches, int fbw_channels, int coded_channels) {
    std::string out;
    for (const auto& mismatch : mismatches) {
        if (!out.empty()) {
            out += "\n";
        }
        out += describe(mismatch, fbw_channels, coded_channels);
    }
    return out;
}

}  // namespace ac3::verify
