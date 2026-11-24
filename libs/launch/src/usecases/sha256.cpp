// SPDX-License-Identifier: MIT
#include "usecases/sha256.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace vehicle_sil::launch::detail {

namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants{
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

constexpr std::uint32_t load_be32(const std::uint8_t* p) noexcept
{
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

void compress_block(std::array<std::uint32_t, 8>& state, const std::uint8_t* block) noexcept
{
    std::array<std::uint32_t, 64> w{};
    for (std::size_t t = 0; t < 16; ++t)
    {
        w[t] = load_be32(block + t * 4);
    }
    for (std::size_t t = 16; t < 64; ++t)
    {
        const std::uint32_t s0 = std::rotr(w[t - 15], 7) ^ std::rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
        const std::uint32_t s1 = std::rotr(w[t - 2], 17) ^ std::rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
        w[t] = w[t - 16] + s0 + w[t - 7] + s1;
    }

    std::uint32_t a = state[0];
    std::uint32_t b = state[1];
    std::uint32_t c = state[2];
    std::uint32_t d = state[3];
    std::uint32_t e = state[4];
    std::uint32_t f = state[5];
    std::uint32_t g = state[6];
    std::uint32_t h = state[7];

    for (std::size_t t = 0; t < 64; ++t)
    {
        const std::uint32_t big_s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ ((~e) & g);
        const std::uint32_t temp1 = h + big_s1 + ch + kRoundConstants[t] + w[t];
        const std::uint32_t big_s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = big_s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

} // namespace

std::string sha256_hex(std::string_view data)
{
    std::array<std::uint32_t, 8> state{
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };

    std::size_t offset = 0;
    while (data.size() - offset >= 64)
    {
        std::array<std::uint8_t, 64> block{};
        for (std::size_t i = 0; i < 64; ++i)
        {
            block[i] = static_cast<std::uint8_t>(data[offset + i]);
        }
        compress_block(state, block.data());
        offset += 64;
    }

    std::array<std::uint8_t, 64> tail{};
    const std::size_t rem = data.size() - offset;
    for (std::size_t i = 0; i < rem; ++i)
    {
        tail[i] = static_cast<std::uint8_t>(data[offset + i]);
    }
    tail[rem] = 0x80u;
    if (rem >= 56)
    {
        compress_block(state, tail.data());
        tail.fill(0);
    }

    const std::uint64_t bit_len = static_cast<std::uint64_t>(data.size()) * 8u;
    for (std::size_t i = 0; i < 8; ++i)
    {
        tail[56 + i] = static_cast<std::uint8_t>((bit_len >> ((7 - i) * 8)) & 0xffu);
    }
    compress_block(state, tail.data());

    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (const std::uint32_t word : state)
    {
        for (int shift = 28; shift >= 0; shift -= 4)
        {
            out.push_back(kHex[(word >> shift) & 0xfu]);
        }
    }
    return out;
}

} // namespace vehicle_sil::launch::detail
