#include "ac3/emdf/emdf.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "ac3/core/bitreader.hpp"
#include "ac3/core/bitwriter.hpp"

namespace ac3::emdf {

namespace {

// The group count and the offset that goes with it, per Table H.2.1. Values
// below 2^n take one group; the next 2^2n values take two, and so on.
struct VarBitsShape {
    int groups = 1;
    std::uint64_t offset = 0;
};

[[nodiscard]] VarBitsShape variable_bits_shape(std::uint32_t value, int group_bits) {
    assert(group_bits > 0 && group_bits <= 11);
    VarBitsShape shape;
    while (true) {
        const std::uint64_t capacity = std::uint64_t{1} << (shape.groups * group_bits);
        if (value < shape.offset + capacity) {
            return shape;
        }
        shape.offset += capacity;
        ++shape.groups;
    }
}

// §H.2.2.3, restricted to TS 103 420 Table 56. Every field there is fixed, so
// the only degree of freedom is groupid.
//
// Except codecdatae, where the two standards contradict each other: Table 56
// says 1, and §H.2.2.3.7 says it "shall be set to '0'" for configurations
// conforming to Annex H. Dolby's own reference streams settle it - they send
// 0. Sending 1 costs eight reserved bits, and since emdf_payload_config has no
// length of its own, those eight bits shift every field after them; a decoder
// reading the other convention gets a garbage payload size and abandons the
// container. So the tie goes to the value that is actually on the wire.
void put_payload_config(BitWriter& w, int groupid) {
    w.put(0, 1);  // smploffste: the payload applies from the frame's first sample
    w.put(0, 1);  // duratione: ... to its last
    w.put(1, 1);  // groupide
    put_variable_bits(w, static_cast<std::uint32_t>(groupid), 2);
    w.put(0, 1);  // codecdatae
    w.put(0, 1);  // discard_unknown_payload
    // discard_unknown_payload == 0 with smploffste == 0 opens the alignment
    // branch; frame alignment then opens priority and proc_allowed.
    w.put(1, 1);  // payload_frame_aligned
    w.put(0, 1);  // create_duplicate
    w.put(0, 1);  // remove_duplicate
    w.put(0, 5);  // priority: 0 is the highest
    w.put(0, 2);  // proc_allowed: discard if anything is processed
}

}  // namespace

void put_variable_bits(BitWriter& w, std::uint32_t value, int group_bits) {
    const auto [groups, offset] = variable_bits_shape(value, group_bits);
    const auto encoded = static_cast<std::uint64_t>(value) - offset;
    for (int group = groups - 1; group >= 0; --group) {
        const auto shift = group * group_bits;
        w.put(static_cast<std::uint32_t>((encoded >> shift) &
                                         ((std::uint64_t{1} << group_bits) - 1)),
              group_bits);
        w.put(group == 0 ? 0u : 1u, 1);  // read_more
    }
}

int variable_bits_size(std::uint32_t value, int group_bits) {
    return variable_bits_shape(value, group_bits).groups * (group_bits + 1);
}

namespace {

// Decode-side inverse of put_variable_bits: groups of `group_bits`, MSB
// group first, each followed by a read_more bit, accumulating the same
// per-group offset the writer folds in. BitReader's sticky overflow makes
// this safe against a stream that never sends a 0 read_more bit - once past
// the end, read_bit() returns 0 forever, so read_more reads false and the
// loop terminates instead of running away.
std::uint32_t read_variable_bits(BitReader& r, int group_bits) {
    std::uint32_t value = 0;
    while (true) {
        value += r.read(static_cast<int>(group_bits));
        if (r.read_bit() == 0) {
            return value;
        }
        value <<= group_bits;
        value += 1u << group_bits;
    }
}

// §H.2.2.1.1: the container's position depends on how many bits the audio
// took, so it is found by scanning rather than at a fixed offset. Mirrors
// tests/emdf/test_emdf.cpp's own find_emdf_sync, promoted here because production
// decode needs the same search, not just a test helper for one.
constexpr std::size_t kSyncNotFound = static_cast<std::size_t>(-1);

std::size_t find_sync_bit(std::span<const std::byte> data) {
    const std::size_t total_bits = data.size() * 8;
    if (total_bits < 16) {
        return kSyncNotFound;
    }
    for (std::size_t bit = 0; bit + 16 <= total_bits; ++bit) {
        BitReader r{data};
        r.skip(bit);
        if (r.read(16) == kSyncWord) {
            return bit;
        }
    }
    return kSyncNotFound;
}

// Decode-side inverse of put_payload_config, restricted (like the writer) to
// the one shape TS 103 420 Table 56 mandates. Returns false for anything
// else - an `e`/mode bit reading differently from what the writer always
// sends - since the fields that would follow a different choice are not
// documented anywhere this codebase transcribes them from, and guessing
// their width would risk desynchronising the rest of the container.
[[nodiscard]] bool read_payload_config(BitReader& r) {
    if (r.read(1) != 0) {  // smploffste
        return false;
    }
    if (r.read(1) != 0) {  // duratione
        return false;
    }
    if (r.read(1) != 1) {  // groupide
        return false;
    }
    read_variable_bits(r, 2);  // groupid: not needed to tell OAMD and JOC apart, both arrive by id
    if (r.read(1) != 0) {  // codecdatae - see put_payload_config's own comment
        return false;
    }
    if (r.read(1) != 0) {  // discard_unknown_payload
        return false;
    }
    if (r.read(1) != 1) {  // payload_frame_aligned
        return false;
    }
    r.skip(1);  // create_duplicate
    r.skip(1);  // remove_duplicate
    r.skip(5);  // priority
    r.skip(2);  // proc_allowed
    return true;
}

}  // namespace

std::vector<std::byte> build_container(std::span<const Payload> payloads, int groupid) {
    BitWriter container;
    container.put(0, 2);  // emdf_version: 0 is the syntax in Annex H
    container.put(0, 3);  // key_id: no authentication key

    for (const auto& payload : payloads) {
        assert(payload.id > 0 && payload.id < 0x1F);  // 0 terminates; 0x1F escapes
        container.put(static_cast<std::uint32_t>(payload.id), 5);
        put_payload_config(container, groupid);
        put_variable_bits(container, static_cast<std::uint32_t>(payload.bytes.size()), 8);
        for (const auto byte : payload.bytes) {
            container.put(std::to_integer<std::uint32_t>(byte), 8);
        }
    }
    container.put(0, 5);  // emdf_payload_id == 0x0 closes the payload list

    // §H.2.2.4. What the protection bits actually contain is "implementation
    // dependent and not defined", so no decoder can check them against an
    // algorithm it does not know - zero-filled is both compliant and honest
    // regardless of width. But width itself is not free: real Dolby encoders
    // reserve 32 bits primary + 8 bits secondary (confirmed against an
    // unmodified commercial Atmos-in-DD+ disc, byte-identical shape to this),
    // and a decoder that validates the field at all reads exactly that many
    // bits as the tag - a shorter reservation leaves nowhere for a real,
    // full-length tag to go. This clean-room encoder still cannot produce
    // that tag itself (the algorithm is undefined-by-spec, i.e. Dolby's to
    // know), so these bits stay zero here regardless; matching the width is
    // what leaves room for the optional, local-only quarantine signer (see
    // src/quarantine/README.md) to fill them in correctly after the fact,
    // without the codec depending on it in any way - an unsigned build's
    // stream is bit-for-bit the same shape either way, just zero-filled.
    container.put(0b10, 2);  // protection_length_primary: 32 bits
    container.put(0b01, 2);  // protection_length_secondary: 8 bits
    container.put(0, 32);    // protection_bits_primary
    container.put(0, 8);     // protection_bits_secondary

    // take() zero-pads to the byte boundary, which is exactly what §H.2.2.1.2
    // asks for: the bits between the end of the container and the boundary
    // implied by emdf_container_length shall be zero.
    const std::vector<std::byte> body = container.take();

    BitWriter out;
    out.put(kSyncWord, 16);
    // §H.2.2.1.2 measures "the EMDF container", which emdf_sync() precedes
    // rather than belongs to, so the four bytes of sync are not counted.
    out.put(static_cast<std::uint32_t>(body.size()), 16);
    for (const auto byte : body) {
        out.put(std::to_integer<std::uint32_t>(byte), 8);
    }
    return out.take();
}

std::expected<std::optional<std::vector<DecodedPayload>>, ParseError> parse_container(
    std::span<const std::byte> data) {
    const std::size_t sync_bit = find_sync_bit(data);
    if (sync_bit == kSyncNotFound) {
        return std::optional<std::vector<DecodedPayload>>{std::nullopt};
    }

    BitReader r{data};
    r.skip(sync_bit + 16);  // past the sync word itself, already matched
    const auto length = r.read(16);
    const std::size_t total_bits = data.size() * 8;
    // §H.2.2.1.2: emdf_container_length counts the container in bytes,
    // starting right after this field - the sync word precedes it and is not
    // part of the count.
    if (sync_bit + 32 + static_cast<std::size_t>(length) * 8 > total_bits) {
        return std::unexpected(ParseError::kTruncated);
    }

    if (r.read(2) != 0) {  // emdf_version: only 0 (Annex H's own syntax) is defined
        return std::unexpected(ParseError::kUnsupportedConfig);
    }
    r.skip(3);  // key_id: unauthenticated, nothing to check on decode

    std::vector<DecodedPayload> payloads;
    while (true) {
        const auto id = r.read(5);
        if (id == 0) {  // terminates the payload list
            break;
        }
        if (id == 0x1F) {  // §H.2.2.2.2: the size-extension escape, never emitted here
            return std::unexpected(ParseError::kUnsupportedConfig);
        }
        if (!read_payload_config(r)) {
            return std::unexpected(ParseError::kUnsupportedConfig);
        }
        const auto size = read_variable_bits(r, 8);
        // Attacker/corruption-controlled: bound it against what is actually
        // left before allocating, rather than trusting a field that could
        // otherwise claim gigabytes from a few dozen bits of garbage.
        const std::size_t remaining_bits = total_bits > r.bit_position() ? total_bits - r.bit_position() : 0;
        if (static_cast<std::size_t>(size) * 8 > remaining_bits) {
            return std::unexpected(ParseError::kTruncated);
        }
        DecodedPayload payload;
        payload.id = static_cast<int>(id);
        payload.bytes.reserve(size);
        for (std::uint32_t i = 0; i < size; ++i) {
            payload.bytes.push_back(static_cast<std::byte>(r.read(8)));
        }
        payloads.push_back(std::move(payload));
    }

    // §H.2.2.4: skip the protection field rather than validate it - its
    // content is implementation dependent and unverifiable, see
    // build_container's own comment. Only the shape real streams (this
    // encoder's and Dolby's own reference ones) actually use is recognised;
    // anything else is refused rather than mis-skipped.
    if (r.read(2) != 0b10 || r.read(2) != 0b01) {
        return std::unexpected(ParseError::kUnsupportedConfig);
    }
    r.skip(32);  // protection_bits_primary
    r.skip(8);   // protection_bits_secondary

    if (r.overflowed()) {
        return std::unexpected(ParseError::kTruncated);
    }
    return std::optional<std::vector<DecodedPayload>>{std::move(payloads)};
}

}  // namespace ac3::emdf
