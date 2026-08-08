#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Object Audio Metadata - ETSI TS 103 420 clause 5. The payload that says what
// the objects ARE: how many, where each one sits in the room, how loud, and
// which of them are nailed to speakers rather than free to move.
//
// OAMD carries no audio. The essences come out of the JOC decoder, one per
// object, and OAMD is matched to them BY POSITION (§4.3 decodes the essences
// at step 3 and the properties at step 5, and never pairs them by name) - so
// object order is a wire contract, not a presentation detail. §5.6.4.8 fixes
// it: objects contained in a bed first, then ISF objects, then dynamic ones.
//
// Two channel orderings collide here and they are not the same:
//   - a bed's own order is Table 12's, read from array index 9 downwards, so
//     5.1 is L, R, C, LFE, Ls, Rs;
//   - AC-3 codes 3/2 + LFE as L, C, R, Ls, Rs, LFE (Table 5.8).
// C and R swap and the LFE moves. The JOC downmix (Table 47) agrees with the
// former, so the encoder permutes once on the way in and everything object-
// facing stays in Table 12 order.

namespace ac3::oba {

// §4.2.1's room-anchored system: left-handed and normalized to the room
// cuboid. x runs 0 at the left wall to 1 at the right, y 0 at the front wall
// to 1 at the back, z -1 at the floor to +1 at the ceiling. The centre of the
// front wall is (0.5, 0, 0).
struct Position {
    double x = 0.5;
    double y = 0.5;
    double z = 0.0;
};

// An object placed in the room rather than assigned to a speaker.
struct DynamicObject {
    Position position{};
    // §5.6.1.4. Table 19 covers [-49, -1] and [1, 15] dB; exactly 0 dB is
    // unreachable through object_gain_bits and is sent as object_gain_idx 0
    // instead, which is why this is a dB figure rather than an index.
    double gain_db = 0.0;
};

// §5.6.1.1.4 Table 12. Each flag names one channel label or a PAIR of them, so
// a set bit is not always one channel. The array index IS the bit position,
// index 9 being the most significant of the 10-bit field.
namespace bed {

inline constexpr std::uint16_t kLR = 1 << 9;      // 2 channels
inline constexpr std::uint16_t kC = 1 << 8;
inline constexpr std::uint16_t kLfe = 1 << 7;
inline constexpr std::uint16_t kLsRs = 1 << 6;    // 2
inline constexpr std::uint16_t kLbRb = 1 << 5;    // 2
inline constexpr std::uint16_t kTflTfr = 1 << 4;  // 2
inline constexpr std::uint16_t kTslTsr = 1 << 3;  // 2
inline constexpr std::uint16_t kTblTbr = 1 << 2;  // 2
inline constexpr std::uint16_t kLwRw = 1 << 1;    // 2
inline constexpr std::uint16_t kLfe2 = 1 << 0;

inline constexpr std::uint16_t kPairs =
    kLR | kLsRs | kLbRb | kTflTfr | kTslTsr | kTblTbr | kLwRw;

inline constexpr std::uint16_t k51 = kLR | kC | kLfe | kLsRs;

[[nodiscard]] constexpr int channel_count(std::uint16_t assignment) {
    int count = 0;
    for (int bit = 0; bit < 10; ++bit) {
        if (assignment & (1 << bit)) {
            count += (assignment & kPairs & (1 << bit)) ? 2 : 1;
        }
    }
    return count;
}

static_assert(channel_count(k51) == 6);
static_assert(channel_count(kLfe) == 1);

}  // namespace bed

// The program: one optional bed instance, then some dynamic objects.
//
// The default is an LFE-ONLY bed alongside the objects, which is the shape
// this encoder's spatial layer actually produces. The reason is
// double-counting. A renderer sums the bed and the objects, so if the 5.1
// downmix is the VBAP render of the objects and the program ALSO declares
// those five channels as a bed, every object arrives twice. Declaring only
// the objects (plus the LFE, which no panner feeds) says exactly what is
// there. §5.6.1.1.6's b_lfe_only exists for precisely this bed.
//
// A dynamic-object-only program (§5.6.0.5) would say nearly the same thing,
// but its branch of program_assignment signals neither the object count nor
// where the LFE sits in the object order, leaving both to be inferred. The
// LFE-only bed instance leaves nothing to inference.
struct Program {
    std::uint16_t bed = bed::kLfe;  // 0 for no bed instance at all
    int dynamic_objects = 0;
};

// Objects in the program, bed first. This is object_count in the payload and
// complexity_index_type_a in addbsi (TS 103 420 §8.3.2.2).
[[nodiscard]] constexpr int object_count(const Program& program) {
    return bed::channel_count(program.bed) + program.dynamic_objects;
}

// Objects the JOC tool has to reconstruct. §6.3.2.2's note bypasses the LFE
// rather than matrixing it, so it costs no JOC output even though it is an
// object like any other.
[[nodiscard]] constexpr int joc_object_count(const Program& program) {
    return object_count(program) - ((program.bed & bed::kLfe) ? 1 : 0) -
           ((program.bed & bed::kLfe2) ? 1 : 0);
}

// One object_audio_metadata_payload (§5.5.2), padded to whole bytes because
// emdf_payload_size counts bytes. `objects` describes the dynamic objects in
// order; the bed's are implied by the channel assignment and are sent at unity
// gain and default priority.
[[nodiscard]] std::vector<std::byte> build_payload(const Program& program,
                                                   std::span<const DynamicObject> objects);

}  // namespace ac3::oba
