#include "ac3/emdf/emdf.hpp"

#include <cassert>

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
    // algorithm it does not know; the shortest legal primary field carrying
    // zero is therefore both compliant and honest. 0b00 is reserved for the
    // primary length, so eight bits is the floor.
    container.put(0b01, 2);  // protection_length_primary: 8 bits
    container.put(0b00, 2);  // protection_length_secondary: absent
    container.put(0, 8);     // protection_bits_primary

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

}  // namespace ac3::emdf
