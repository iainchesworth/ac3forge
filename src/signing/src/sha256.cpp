#include "sha256.hpp"

#include <algorithm>
#include <cstring>

namespace ac3::signing {
namespace {

constexpr std::array<std::uint32_t, 8> kInitialHash = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
    0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
    0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
    0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
    0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
    0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};

constexpr std::uint32_t ror(std::uint32_t x, int r) {
    return (x >> r) | (x << (32 - r));
}

}  // namespace

void Sha256::reset() {
    h_ = kInitialHash;
    total_bytes_ = 0;
    buffered_ = 0;
}

void Sha256::process_block(const std::uint8_t* block) {
    std::array<std::uint32_t, 64> w{};
    for (int i = 0; i < 16; ++i) {
        w[static_cast<std::size_t>(i)] =
            (std::uint32_t(block[i * 4]) << 24) | (std::uint32_t(block[i * 4 + 1]) << 16) |
            (std::uint32_t(block[i * 4 + 2]) << 8) | std::uint32_t(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 = ror(w[static_cast<std::size_t>(i - 15)], 7) ^
                                 ror(w[static_cast<std::size_t>(i - 15)], 18) ^
                                 (w[static_cast<std::size_t>(i - 15)] >> 3);
        const std::uint32_t s1 = ror(w[static_cast<std::size_t>(i - 2)], 17) ^
                                 ror(w[static_cast<std::size_t>(i - 2)], 19) ^
                                 (w[static_cast<std::size_t>(i - 2)] >> 10);
        w[static_cast<std::size_t>(i)] = w[static_cast<std::size_t>(i - 16)] + s0 +
                                         w[static_cast<std::size_t>(i - 7)] + s1;
    }

    std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
    std::uint32_t e = h_[4], f = h_[5], g = h_[6], hh = h_[7];
    for (int i = 0; i < 64; ++i) {
        const std::uint32_t s1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t t1 = hh + s1 + ch + kRoundConstants[static_cast<std::size_t>(i)] +
                                 w[static_cast<std::size_t>(i)];
        const std::uint32_t s0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = s0 + maj;
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
    h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += hh;
}

void Sha256::update(std::span<const std::byte> data) {
    total_bytes_ += data.size();
    const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
    std::size_t len = data.size();
    while (len != 0) {
        const std::size_t take = std::min(std::size_t{64} - buffered_, len);
        std::memcpy(buffer_.data() + buffered_, p, take);
        buffered_ += take;
        p += take;
        len -= take;
        if (buffered_ == 64) {
            process_block(buffer_.data());
            buffered_ = 0;
        }
    }
}

void Sha256::finish(std::span<std::byte, 32> out) {
    const std::uint64_t bit_length = total_bytes_ * 8;

    const std::byte pad_start{0x80};
    update({&pad_start, 1});
    const std::byte zero{0};
    while (buffered_ != 56) {
        update({&zero, 1});
    }
    std::array<std::byte, 8> length_be{};
    for (int i = 0; i < 8; ++i) {
        length_be[static_cast<std::size_t>(i)] =
            static_cast<std::byte>(bit_length >> (56 - i * 8));
    }
    update(length_be);

    for (int i = 0; i < 8; ++i) {
        out[static_cast<std::size_t>(i * 4)] = static_cast<std::byte>(h_[static_cast<std::size_t>(i)] >> 24);
        out[static_cast<std::size_t>(i * 4 + 1)] = static_cast<std::byte>(h_[static_cast<std::size_t>(i)] >> 16);
        out[static_cast<std::size_t>(i * 4 + 2)] = static_cast<std::byte>(h_[static_cast<std::size_t>(i)] >> 8);
        out[static_cast<std::size_t>(i * 4 + 3)] = static_cast<std::byte>(h_[static_cast<std::size_t>(i)]);
    }
    reset();
}

std::array<std::byte, 32> sha256(std::span<const std::byte> data) {
    Sha256 hash;
    hash.update(data);
    std::array<std::byte, 32> out{};
    hash.finish(out);
    return out;
}

}  // namespace ac3::signing
