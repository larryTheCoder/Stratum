// Stratum — the block-level biome zoom (biome/voronoi.hpp).
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// ADAPTED (CLAUDE.md, NOTICE) from cubiomes (https://github.com/Cubitect/
// cubiomes, MIT License, Copyright (c) 2020 Cubitect), at
// e61f90580cbdd883214a8054670dacae655e59c0:
//
//   * obfuscatedSeed: layers.c `getVoronoiSHA`, a single-block SHA-256 of
//     the seed's eight little-endian bytes, its round constants and initial
//     state the standard ones (FIPS 180-4);
//   * jitter: layers.c `getVoronoiCell`, with rng.h `mcStepSeed`;
//   * zoom: layers.c `voronoiAccess3D`.
//
// Recorded in NOTICE.

#include <stratum/biome/voronoi.hpp>
#include <stratum/javamath.hpp>

#include <array>
#include <bit>
#include <cstdint>
#include <limits>

namespace stratum::biome {

namespace {

/// One step of the zoom's LCG: `s * (s * A + C) + salt`, every operation
/// wrapping at 64 bits (rng.h `mcStepSeed`).
[[nodiscard]] constexpr std::uint64_t step(const std::uint64_t s,
                                           const std::uint64_t salt) noexcept {
    constexpr std::uint64_t kMultiplier = 6364136223846793005ULL;
    constexpr std::uint64_t kIncrement = 1442695040888963407ULL;
    return (s * ((s * kMultiplier) + kIncrement)) + salt;
}

/// A 32-bit coordinate as the step's salt: sign-extended to 64 bits, as C's
/// int-to-uint64_t conversion does.
[[nodiscard]] constexpr std::uint64_t salt(const std::int32_t value) noexcept {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(value));
}

/// `((s >> 24) & 1023) - 512) * 36`: one jitter component, from the state's
/// bits 24..33 (an unsigned shift of the unsigned state).
[[nodiscard]] constexpr std::int32_t component(const std::uint64_t s) noexcept {
    return (static_cast<std::int32_t>((s >> 24U) & 1023U) - 512) * 36;
}

/// A 32-bit byte swap (std::byteswap is C++23).
[[nodiscard]] constexpr std::uint32_t swap32(const std::uint32_t value) noexcept {
    return ((value & 0x000000FFU) << 24U) | ((value & 0x0000FF00U) << 8U) |
           ((value & 0x00FF0000U) >> 8U) | ((value & 0xFF000000U) >> 24U);
}

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

constexpr std::array<std::uint32_t, 8> kInitialState = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
};

} // namespace

std::uint64_t VoronoiZoom::obfuscatedSeed(const std::int64_t worldSeed) noexcept {
    const auto seed = static_cast<std::uint64_t>(worldSeed);

    // The one 64-byte block of an 8-byte message: the bytes little-endian
    // (so each 32-bit big-endian word is a byte-swapped half), the 0x80 pad
    // byte, and the bit length, 64, in the last word.
    std::array<std::uint32_t, 64> words{};
    words[0] = swap32(static_cast<std::uint32_t>(seed));
    words[1] = swap32(static_cast<std::uint32_t>(seed >> 32U));
    words[2] = 0x80000000U;
    words[15] = 64U;
    for (std::size_t i = 16; i < 64; ++i) {
        const std::uint32_t w15 = words[i - 15];
        const std::uint32_t w2 = words[i - 2];
        words[i] = words[i - 7] + words[i - 16] +
                   (std::rotr(w15, 7) ^ std::rotr(w15, 18) ^ (w15 >> 3U)) +
                   (std::rotr(w2, 17) ^ std::rotr(w2, 19) ^ (w2 >> 10U));
    }

    std::array<std::uint32_t, 8> s = kInitialState;
    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t t1 = s[7] + kRoundConstants[i] + words[i] +
                                 (std::rotr(s[4], 6) ^ std::rotr(s[4], 11) ^ std::rotr(s[4], 25)) +
                                 ((s[4] & s[5]) ^ (~s[4] & s[6]));
        const std::uint32_t t2 = (std::rotr(s[0], 2) ^ std::rotr(s[0], 13) ^ std::rotr(s[0], 22)) +
                                 ((s[0] & s[1]) ^ (s[0] & s[2]) ^ (s[1] & s[2]));
        s[7] = s[6];
        s[6] = s[5];
        s[5] = s[4];
        s[4] = s[3] + t1;
        s[3] = s[2];
        s[2] = s[1];
        s[1] = s[0];
        s[0] = t1 + t2;
    }

    // The digest's first eight bytes, read little-endian.
    const std::uint32_t first = s[0] + kInitialState[0];
    const std::uint32_t second = s[1] + kInitialState[1];
    return static_cast<std::uint64_t>(swap32(first)) |
           (static_cast<std::uint64_t>(swap32(second)) << 32U);
}

std::array<std::int32_t, 3> VoronoiZoom::jitter(const std::int32_t x, const std::int32_t y,
                                                const std::int32_t z) const noexcept {
    std::uint64_t s = seed_;
    s = step(s, salt(x));
    s = step(s, salt(y));
    s = step(s, salt(z));
    s = step(s, salt(x));
    s = step(s, salt(y));
    s = step(s, salt(z));
    const std::int32_t jx = component(s);
    s = step(s, seed_);
    const std::int32_t jy = component(s);
    s = step(s, seed_);
    const std::int32_t jz = component(s);
    return {jx, jy, jz};
}

QuartPos VoronoiZoom::zoom(const std::int32_t x, const std::int32_t y,
                           const std::int32_t z) const noexcept {
    // The lattice is offset by two blocks: the block sits in the cell of
    // (coordinate - 2), and its place in that cell, in 1/10240ths of a quart.
    // In int64, so a coordinate at the int32 edge cannot overflow the shift.
    const std::int64_t sx = std::int64_t{x} - 2;
    const std::int64_t sy = std::int64_t{y} - 2;
    const std::int64_t sz = std::int64_t{z} - 2;
    const std::int64_t px = javamath::floorDiv(sx, std::int64_t{4});
    const std::int64_t py = javamath::floorDiv(sy, std::int64_t{4});
    const std::int64_t pz = javamath::floorDiv(sz, std::int64_t{4});
    const std::int64_t dx = javamath::floorMod(sx, std::int64_t{4}) * 10240;
    const std::int64_t dy = javamath::floorMod(sy, std::int64_t{4}) * 10240;
    const std::int64_t dz = javamath::floorMod(sz, std::int64_t{4}) * 10240;

    QuartPos best;
    std::int64_t nearest = std::numeric_limits<std::int64_t>::max();
    bool first = true;
    for (int i = 0; i < 8; ++i) {
        const std::int64_t bx = (i & 4) != 0 ? 1 : 0;
        const std::int64_t by = (i & 2) != 0 ? 1 : 0;
        const std::int64_t bz = (i & 1) != 0 ? 1 : 0;
        const auto cx = static_cast<std::int32_t>(px + bx);
        const auto cy = static_cast<std::int32_t>(py + by);
        const auto cz = static_cast<std::int32_t>(pz + bz);
        const std::array<std::int32_t, 3> j = jitter(cx, cy, cz);
        const std::int64_t rx = j[0] + dx - (40960 * bx);
        const std::int64_t ry = j[1] + dy - (40960 * by);
        const std::int64_t rz = j[2] + dz - (40960 * bz);
        const std::int64_t distance = (rx * rx) + (ry * ry) + (rz * rz);
        // Strictly nearer: the first of equals keeps it (cubiomes' `d < dmin`
        // from an unsigned maximum, which the first candidate always beats).
        if (first || distance < nearest) {
            nearest = distance;
            best = QuartPos{.x = cx, .y = cy, .z = cz};
            first = false;
        }
    }
    return best;
}

VoronoiWindow::VoronoiWindow(const VoronoiZoom& zoom, const std::int32_t minX,
                             const std::int32_t minY, const std::int32_t minZ,
                             const std::int32_t maxX, const std::int32_t maxY,
                             const std::int32_t maxZ)
    : zoom_(&zoom), minX_(minX), minY_(minY), minZ_(minZ), maxX_(maxX), maxY_(maxY), maxZ_(maxZ),
      latticeX_(
          static_cast<std::int32_t>(javamath::floorDiv(std::int64_t{minX} - 2, std::int64_t{4}))),
      latticeY_(
          static_cast<std::int32_t>(javamath::floorDiv(std::int64_t{minY} - 2, std::int64_t{4}))),
      latticeZ_(
          static_cast<std::int32_t>(javamath::floorDiv(std::int64_t{minZ} - 2, std::int64_t{4}))),
      spanX_(static_cast<std::size_t>(javamath::floorDiv(std::int64_t{maxX} - 2, std::int64_t{4}) -
                                      latticeX_ + 2)),
      spanY_(static_cast<std::size_t>(javamath::floorDiv(std::int64_t{maxY} - 2, std::int64_t{4}) -
                                      latticeY_ + 2)),
      spanZ_(static_cast<std::size_t>(javamath::floorDiv(std::int64_t{maxZ} - 2, std::int64_t{4}) -
                                      latticeZ_ + 2)),
      jitter_(spanX_ * spanY_ * spanZ_) {
    for (std::size_t lx = 0; lx < spanX_; ++lx) {
        for (std::size_t ly = 0; ly < spanY_; ++ly) {
            for (std::size_t lz = 0; lz < spanZ_; ++lz) {
                jitter_[(((lx * spanY_) + ly) * spanZ_) + lz] =
                    zoom.jitter(latticeX_ + static_cast<std::int32_t>(lx),
                                latticeY_ + static_cast<std::int32_t>(ly),
                                latticeZ_ + static_cast<std::int32_t>(lz));
            }
        }
    }
}

const std::array<std::int32_t, 3>& VoronoiWindow::at(const std::int64_t lx, const std::int64_t ly,
                                                     const std::int64_t lz) const noexcept {
    return jitter_[(((static_cast<std::size_t>(lx) * spanY_) + static_cast<std::size_t>(ly)) *
                    spanZ_) +
                   static_cast<std::size_t>(lz)];
}

QuartPos VoronoiWindow::zoom(const std::int32_t x, const std::int32_t y,
                             const std::int32_t z) noexcept {
    if (x < minX_ || x > maxX_ || y < minY_ || y > maxY_ || z < minZ_ || z > maxZ_) {
        return zoom_->zoom(x, y, z);
    }
    // VoronoiZoom::zoom, term for term, with the jitters read from the table
    // and the x and z terms kept across a column's lattice layer.
    const std::int64_t sx = std::int64_t{x} - 2;
    const std::int64_t sy = std::int64_t{y} - 2;
    const std::int64_t sz = std::int64_t{z} - 2;
    const std::int64_t px = javamath::floorDiv(sx, std::int64_t{4});
    const std::int64_t py = javamath::floorDiv(sy, std::int64_t{4});
    const std::int64_t pz = javamath::floorDiv(sz, std::int64_t{4});
    if (!horizontalKnown_ || lastX_ != x || lastZ_ != z || lastLayer_ != py) {
        const std::int64_t dx = javamath::floorMod(sx, std::int64_t{4}) * 10240;
        const std::int64_t dz = javamath::floorMod(sz, std::int64_t{4}) * 10240;
        for (int i = 0; i < 8; ++i) {
            const std::int64_t bx = (i & 4) != 0 ? 1 : 0;
            const std::int64_t by = (i & 2) != 0 ? 1 : 0;
            const std::int64_t bz = (i & 1) != 0 ? 1 : 0;
            const std::array<std::int32_t, 3>& j =
                at(px + bx - latticeX_, py + by - latticeY_, pz + bz - latticeZ_);
            const std::int64_t rx = j[0] + dx - (40960 * bx);
            const std::int64_t rz = j[2] + dz - (40960 * bz);
            horizontal_[static_cast<std::size_t>(i)] = (rx * rx) + (rz * rz);
            jitterY_[static_cast<std::size_t>(i)] = j[1];
        }
        horizontalKnown_ = true;
        lastX_ = x;
        lastZ_ = z;
        lastLayer_ = static_cast<std::int32_t>(py);
    }
    const std::int64_t dy = javamath::floorMod(sy, std::int64_t{4}) * 10240;
    int best = 0;
    std::int64_t nearest = 0;
    for (int i = 0; i < 8; ++i) {
        const std::int64_t by = (i & 2) != 0 ? 1 : 0;
        const std::int64_t ry = jitterY_[static_cast<std::size_t>(i)] + dy - (40960 * by);
        // (rx*rx + ry*ry) + rz*rz in zoom() and rx*rx + rz*rz + ry*ry here:
        // the same integers, exact in int64, so the order cannot matter.
        const std::int64_t distance = horizontal_[static_cast<std::size_t>(i)] + (ry * ry);
        if (i == 0 || distance < nearest) {
            nearest = distance;
            best = i;
        }
    }
    return QuartPos{.x = static_cast<std::int32_t>(px + ((best & 4) != 0 ? 1 : 0)),
                    .y = static_cast<std::int32_t>(py + ((best & 2) != 0 ? 1 : 0)),
                    .z = static_cast<std::int32_t>(pz + ((best & 1) != 0 ? 1 : 0))};
}

} // namespace stratum::biome
