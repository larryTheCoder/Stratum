// Stratum — the carvers, carving one chunk (carver/carve.hpp).
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Claim ids are spec/carvers-a-spec.md's (A-Qn.m) and spec/carvers-b-spec.md's
// (B-Qn.m). Every `f32(...)` of the spec is a float operation here, its
// operands floats; every double one is double. Nothing is reassociated.

#include <stratum/carver/carve.hpp>
#include <stratum/carver/trig.hpp>
#include <stratum/javamath.hpp>
#include <stratum/rng/java_random.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace stratum::carver {

void CarverSet::setBiome(const data::ResourceLocation& biome,
                         std::vector<std::shared_ptr<const CompiledCarver>> carvers) {
    biomes_.insert_or_assign(biome, std::move(carvers));
}

const std::vector<std::shared_ptr<const CompiledCarver>>&
CarverSet::carversOf(const data::ResourceLocation& biome) const noexcept {
    static const std::vector<std::shared_ptr<const CompiledCarver>> kNone;
    const auto found = biomes_.find(biome);
    return found == biomes_.end() ? kNone : found->second;
}

namespace {

using rng::JavaRandom;

constexpr float kTwoPi = 6.2831855F;  // A-Q1.4: f32(2 pi)
constexpr float kPi = 3.1415927F;     // A-Q3.2: f32(pi)
constexpr float kHalfPi = 1.5707964F; // A-Q3.4, A-Q6.1: f32(pi / 2)
constexpr int kTopMargin = 7;         // A-Q4.1 (m; 0 only when upgrading, out of scope)
constexpr std::int32_t kWindow = 8;   // A-Q7.3: the +-8 start window

/// F(): `next(24) * 2^-24`, a float (A-Q1.2). JavaRandom::nextFloat is it.
[[nodiscard]] float draw(JavaRandom& g) {
    return g.nextFloat();
}

/// A-Q1.5 / B-Q1.3: a float provider's value, drawing as it says.
[[nodiscard]] float sample(const FloatProvider& provider, JavaRandom& g) {
    switch (provider.kind) {
        case FloatProvider::Kind::Uniform: {
            // f32((max - min)), then the product with the draw, then + min.
            // Never clamped below max (A-Q1.5's amendment).
            const float span = provider.b - provider.a;
            const float scaled = draw(g) * span;
            return scaled + provider.a;
        }
        case FloatProvider::Kind::Trapezoid: {
            // B-Q1.3: span, slope = (span - plateau) / 2, flat = span - slope,
            // then (min + F() * flat) + F() * slope, left to right.
            const float span = provider.b - provider.a;
            const float slope = (span - provider.c) / 2.0F;
            const float flat = span - slope;
            const float first = draw(g) * flat;
            const float partial = provider.a + first;
            const float second = draw(g) * slope;
            return partial + second;
        }
        case FloatProvider::Kind::Constant:
        default:
            return provider.a;
    }
}

/// A-Q1.5: a uniform height draws `I(hi - lo + 1) + lo`, nothing when
/// lo > hi; a constant draws nothing.
[[nodiscard]] std::int32_t sample(const HeightProvider& provider, JavaRandom& g,
                                  const settings::NoiseGeometry& geometry) {
    const std::int32_t lo = provider.minInclusive.resolve(geometry);
    if (provider.kind == HeightProvider::Kind::Constant) {
        return lo;
    }
    const std::int32_t hi = provider.maxInclusive.resolve(geometry);
    if (lo > hi) {
        return lo;
    }
    const std::int32_t width = javamath::wrappingAdd(javamath::wrappingSub(hi, lo), 1);
    return javamath::wrappingAdd(g.nextInt(width), lo);
}

/// Per target chunk: the mask, the aquifer-flag history, and the writes.
class Carving {
public:
    explicit Carving(const CarveTarget& target)
        : target_(target), x0_(target.chunkX * 16), z0_(target.chunkZ * 16), xm_(x0_ + 8),
          zm_(z0_ + 8), minY_(target.geometry.minY), height_(target.geometry.height),
          mask_(static_cast<std::size_t>(16) * 16 * static_cast<std::size_t>(height_), false) {}

    [[nodiscard]] double xm() const { return static_cast<double>(xm_); }

    [[nodiscard]] double zm() const { return static_cast<double>(zm_); }

    [[nodiscard]] std::int32_t minY() const { return minY_; }

    [[nodiscard]] std::int32_t height() const { return height_; }

    /// A-Q7.1: false when the tunnel must end for this target.
    [[nodiscard]] bool reaches(double x, double z, std::int32_t length, std::int32_t i,
                               float thickness) const {
        const double dx = x - xm();
        const double dz = z - zm();
        const auto r = static_cast<double>(length - i);
        const float qFloat = (thickness + 2.0F) + 16.0F;
        const auto q = static_cast<double>(qFloat);
        return ((dx * dx) + (dz * dz)) - (r * r) <= q * q;
    }

    /// A-Q4.1-Q4.5 (cave cell test) or B-Q4.1 (canyon, with @p widths).
    void ellipsoid(const CompiledCarver& carver, double cx, double cy, double cz, double rho,
                   double nu, double floorLevel, bool hasFloor, const std::vector<float>* widths) {
        // Early-out, speed-only but implemented exactly (A-Q4.1, A-Q8.2).
        if (std::fabs(cx - xm()) > 16.0 + (2.0 * rho) ||
            std::fabs(cz - zm()) > 16.0 + (2.0 * rho)) {
            return;
        }
        const std::int32_t fromX =
            std::max(javamath::floorToInt(cx - rho) - x0_ - 1, std::int32_t{0});
        const std::int32_t toX = std::min(javamath::floorToInt(cx + rho) - x0_, std::int32_t{15});
        const std::int32_t fromZ =
            std::max(javamath::floorToInt(cz - rho) - z0_ - 1, std::int32_t{0});
        const std::int32_t toZ = std::min(javamath::floorToInt(cz + rho) - z0_, std::int32_t{15});
        const std::int32_t yHi =
            std::min(javamath::floorToInt(cy + nu) + 1, minY_ + height_ - 1 - kTopMargin);
        const std::int32_t yLo = std::max(javamath::floorToInt(cy - nu) - 1, minY_ + 1);

        for (std::int32_t lx = fromX; lx <= toX; ++lx) {
            const std::int32_t worldX = x0_ + lx;
            const double u = ((static_cast<double>(worldX) + 0.5) - cx) / rho;
            for (std::int32_t lz = fromZ; lz <= toZ; ++lz) {
                const std::int32_t worldZ = z0_ + lz;
                const double w = ((static_cast<double>(worldZ) + 0.5) - cz) / rho;
                const double horizontal = (u * u) + (w * w);
                if (!(horizontal < 1.0)) {
                    continue;
                }
                bool grassFlag = false; // A-Q12.3: per column, per ellipsoid
                for (std::int32_t y = yHi; y > yLo; --y) {
                    const double t = ((static_cast<double>(y) - 0.5) - cy) / nu;
                    bool excluded = false;
                    if (widths != nullptr) {
                        // B-Q4.1: ((u u + w w) T) + (t t) / 6 >= 1.
                        const auto layer = static_cast<std::size_t>(y - minY_ - 1);
                        const auto width = static_cast<double>((*widths)[layer]);
                        excluded = (horizontal * width) + ((t * t) / 6.0) >= 1.0;
                    } else {
                        // A-Q4.3: (u u + t t) + w w, in that order.
                        excluded =
                            (hasFloor && t <= floorLevel) || ((u * u) + (t * t)) + (w * w) >= 1.0;
                    }
                    if (excluded) {
                        continue;
                    }
                    decide(carver, lx, y, lz, grassFlag);
                }
            }
        }
    }

private:
    [[nodiscard]] std::size_t maskIndex(std::int32_t lx, std::int32_t y, std::int32_t lz) const {
        return (static_cast<std::size_t>(y - minY_) * 256U) + (static_cast<std::size_t>(lz) * 16U) +
               static_cast<std::size_t>(lx);
    }

    /// A-Q4.4 then A-Q12.
    void decide(const CompiledCarver& carver, std::int32_t lx, std::int32_t y, std::int32_t lz,
                bool& grassFlag) {
        const std::size_t index = maskIndex(lx, y, lz);
        if (mask_[index]) {
            return;
        }
        mask_[index] = true;

        const data::ResourceLocation& prior = target_.blockAt(lx, y, lz);
        const bool nether = carver.config.type == CarverType::NetherCave;
        if (!nether && (prior == grassBlock() || prior == mycelium())) {
            grassFlag = true;
        }
        if (!carver.replaceable.contains(prior)) {
            return;
        }

        if (nether) {
            // A-Q12.6: lava at or below y_min + 31, else cave air; no marks.
            target_.write(lx, y, lz,
                          y <= minY_ + 31 ? CarveTarget::Write::Lava : CarveTarget::Write::CaveAir);
            return;
        }

        CarveTarget::Write replacement = CarveTarget::Write::Air;
        const std::int32_t lavaLevel = carver.config.lavaLevel.resolve(target_.geometry);
        if (y <= lavaLevel) {
            // A-Q12.1: lava, the aquifer not consulted; the mark takes the
            // last decision's flag (A-Q12.4).
            replacement = CarveTarget::Write::Lava;
        } else {
            const AquiferAnswer answer = target_.aquifer(x0_ + lx, y, z0_ + lz);
            lastFlag_ = answer.flag;
            switch (answer.kind) {
                case AquiferAnswer::Kind::Solid:
                    return; // the barrier verdict: unchanged (A-Q12.1)
                case AquiferAnswer::Kind::Water:
                    replacement = CarveTarget::Write::Water;
                    break;
                case AquiferAnswer::Kind::Lava:
                    replacement = CarveTarget::Write::Lava;
                    break;
                case AquiferAnswer::Kind::Air:
                default:
                    replacement = CarveTarget::Write::Air;
                    break;
            }
        }
        target_.write(lx, y, lz, replacement);
        const bool fluid =
            replacement == CarveTarget::Write::Water || replacement == CarveTarget::Write::Lava;
        if (fluid && lastFlag_) {
            target_.mark(lx, y, lz);
        }
        // A-Q12.3: the dirt below, rewritten to the top material.
        if (grassFlag && y - 1 >= minY_ && target_.blockAt(lx, y - 1, lz) == dirt()) {
            if (target_.topMaterial(lx, y - 1, lz, fluid)) {
                target_.mark(lx, y - 1, lz);
            }
        }
    }

    [[nodiscard]] static const data::ResourceLocation& grassBlock() {
        static const data::ResourceLocation kId{"minecraft", "grass_block"};
        return kId;
    }

    [[nodiscard]] static const data::ResourceLocation& mycelium() {
        static const data::ResourceLocation kId{"minecraft", "mycelium"};
        return kId;
    }

    [[nodiscard]] static const data::ResourceLocation& dirt() {
        static const data::ResourceLocation kId{"minecraft", "dirt"};
        return kId;
    }

    const CarveTarget& target_;
    std::int32_t x0_;
    std::int32_t z0_;
    std::int32_t xm_;
    std::int32_t zm_;
    std::int32_t minY_;
    std::int32_t height_;
    std::vector<bool> mask_;
    /// A-Q12.4: the aquifer's flag as of its most recent decision; the noise
    /// fill's last decision returns early with it false in every shipped
    /// preset (inferred-from-reading; the golden case confirms it).
    bool lastFlag_ = false;
};

/// The system-level values a cave's tunnels share (A-Q1.4).
struct CaveSystem {
    float horizontalMultiplier = 1.0F;
    float verticalMultiplier = 1.0F;
    float floorLevel = -1.0F;
};

/// A-Q3: one cave tunnel (root or fork), its generator its own.
void caveTunnel(Carving& carving, const CompiledCarver& carver, const CaveSystem& system,
                std::int64_t seed, double x, double y, double z, float theta, float phi,
                float thickness, std::int32_t start, std::int32_t length, double verticalScale) {
    JavaRandom g{seed};
    // A-Q3.1: before the first step, the fork index then the steepness flag.
    const std::int32_t forkAt = g.nextInt(length / 2) + (length / 4);
    const bool steep = g.nextInt(6) == 0;
    float headingVelocity = 0.0F;
    float pitchVelocity = 0.0F;

    for (std::int32_t i = start; i < length; ++i) {
        // A-Q3.2: radii from the index before the step's motion.
        const float angle = (kPi * static_cast<float>(i)) / static_cast<float>(length);
        const float bulge = tableSin(static_cast<double>(angle)) * thickness;
        const double horizontal = 1.5 + static_cast<double>(bulge);
        const double vertical = horizontal * verticalScale;

        // Position.
        const float cosPitch = tableCos(static_cast<double>(phi));
        x += static_cast<double>(tableCos(static_cast<double>(theta)) * cosPitch);
        y += static_cast<double>(tableSin(static_cast<double>(phi)));
        z += static_cast<double>(tableSin(static_cast<double>(theta)) * cosPitch);

        // Pitch, then heading.
        const float damped = phi * (steep ? 0.92F : 0.7F);
        phi = damped + (pitchVelocity * 0.1F);
        theta = theta + (headingVelocity * 0.1F);

        // Velocities decayed, then kicked: all pitch draws before any heading.
        pitchVelocity = pitchVelocity * 0.9F;
        headingVelocity = headingVelocity * 0.75F;
        {
            const float a = draw(g);
            const float b = draw(g);
            const float c = draw(g);
            pitchVelocity = pitchVelocity + (((a - b) * c) * 2.0F);
        }
        {
            const float a = draw(g);
            const float b = draw(g);
            const float c = draw(g);
            headingVelocity = headingVelocity + (((a - b) * c) * 4.0F);
        }

        // A-Q6.1: the fork, with the updated state.
        if (i == forkAt && thickness > 1.0F) {
            const std::int64_t firstSeed = g.nextLong();
            const float firstThickness = (draw(g) * 0.5F) + 0.5F;
            caveTunnel(carving, carver, system, firstSeed, x, y, z, theta - kHalfPi, phi / 3.0F,
                       firstThickness, forkAt, length, 1.0);
            const std::int64_t secondSeed = g.nextLong();
            const float secondThickness = (draw(g) * 0.5F) + 0.5F;
            caveTunnel(carving, carver, system, secondSeed, x, y, z, theta + kHalfPi, phi / 3.0F,
                       secondThickness, forkAt, length, 1.0);
            return;
        }

        // A-Q3.3: the carve check.
        if (g.nextInt(4) == 0) {
            continue;
        }
        if (!carving.reaches(x, z, length, i, thickness)) {
            return;
        }
        carving.ellipsoid(carver, x, y, z,
                          horizontal * static_cast<double>(system.horizontalMultiplier),
                          vertical * static_cast<double>(system.verticalMultiplier),
                          static_cast<double>(system.floorLevel), true, nullptr);
    }
}

/// A-Q1.4: one cave or nether-cave start.
void caveStart(Carving& carving, const CompiledCarver& carver, JavaRandom& g, std::int32_t sx,
               std::int32_t sz, const settings::NoiseGeometry& geometry) {
    const ConfiguredCarver& config = carver.config;
    const bool nether = config.type == CarverType::NetherCave;
    const std::int32_t bound = nether ? 10 : 15;
    const std::int32_t systems = g.nextInt(g.nextInt(g.nextInt(bound) + 1) + 1);
    const double rootVerticalScale = nether ? 5.0 : 1.0;

    for (std::int32_t t = 0; t < systems; ++t) {
        const std::int32_t originX = (sx * 16) + g.nextInt(16);
        const std::int32_t originY = sample(config.y, g, geometry);
        const std::int32_t originZ = (sz * 16) + g.nextInt(16);
        CaveSystem system;
        system.horizontalMultiplier = sample(config.horizontalRadiusMultiplier, g);
        system.verticalMultiplier = sample(config.verticalRadiusMultiplier, g);
        system.floorLevel = sample(config.floorLevel, g);
        const auto x = static_cast<double>(originX);
        const auto y = static_cast<double>(originY);
        const auto z = static_cast<double>(originZ);

        std::int32_t tunnels = 1;
        if (g.nextInt(4) == 0) {
            const float yScale = sample(config.yScale, g);
            const float roomDraw = draw(g) * 6.0F;
            const float radius = 1.0F + roomDraw;
            // A-Q3.4: the room, before the tunnels, from the origin + 1 in x.
            const float bulge = tableSin(static_cast<double>(kHalfPi)) * radius;
            const double roomHorizontal = 1.5 + static_cast<double>(bulge);
            const double roomVertical = roomHorizontal * static_cast<double>(yScale);
            carving.ellipsoid(carver, x + 1.0, y, z, roomHorizontal, roomVertical,
                              static_cast<double>(system.floorLevel), true, nullptr);
            tunnels += g.nextInt(4);
        }

        for (std::int32_t tunnel = 0; tunnel < tunnels; ++tunnel) {
            const float theta = draw(g) * kTwoPi;
            const float pitchDraw = draw(g);
            const float phi = (pitchDraw - 0.5F) / 4.0F;
            float thickness = 0.0F;
            if (nether) {
                const float a = draw(g) * 2.0F;
                const float b = draw(g);
                thickness = (a + b) * 2.0F;
            } else {
                const float a = draw(g) * 2.0F;
                const float base = a + draw(g);
                if (g.nextInt(10) == 0) {
                    const float c = draw(g);
                    const float d = draw(g);
                    thickness = base * (((c * d) * 3.0F) + 1.0F);
                } else {
                    thickness = base;
                }
            }
            const std::int32_t length = 112 - g.nextInt(28);
            const std::int64_t seed = g.nextLong();
            caveTunnel(carving, carver, system, seed, x, y, z, theta, phi, thickness, 0, length,
                       rootVerticalScale);
        }
    }
}

/// B-Q1.2, B-Q3, B-Q10: one canyon start.
void canyonStart(Carving& carving, const CompiledCarver& carver, JavaRandom& g, std::int32_t sx,
                 std::int32_t sz, const settings::NoiseGeometry& geometry) {
    const ConfiguredCarver& config = carver.config;
    const CanyonShape& shape = config.shape;
    auto x = static_cast<double>((sx * 16) + g.nextInt(16));
    auto y = static_cast<double>(sample(config.y, g, geometry));
    auto z = static_cast<double>((sz * 16) + g.nextInt(16));
    float theta = draw(g) * kTwoPi;
    float phi = sample(config.verticalRotation, g);
    const auto yScale = static_cast<double>(sample(config.yScale, g));
    const float thickness = sample(shape.thickness, g);
    const float distance = sample(shape.distanceFactor, g);
    const float scaledLength = 112.0F * distance;
    const std::int32_t length = javamath::doubleToInt(static_cast<double>(scaledLength));
    JavaRandom walk{g.nextLong()};

    // B-Q10.5: the width table, one entry per generation layer.
    std::vector<float> widths(static_cast<std::size_t>(carving.height()));
    float width = 0.0F;
    for (std::size_t j = 0; j < widths.size(); ++j) {
        if (j == 0 || walk.nextInt(shape.widthSmoothness) == 0) {
            const float a = draw(walk);
            const float b = draw(walk);
            width = 1.0F + (a * b);
        }
        widths[j] = width * width;
    }

    float headingVelocity = 0.0F;
    float pitchVelocity = 0.0F;
    for (std::int32_t i = 0; i < length; ++i) {
        // B-Q3.2: radii.
        const float angle = (static_cast<float>(i) * kPi) / static_cast<float>(length);
        const float bulge = tableSin(static_cast<double>(angle)) * thickness;
        const double baseHorizontal = 1.5 + static_cast<double>(bulge);
        const double baseVertical = baseHorizontal * yScale;
        const float horizontalFactor = sample(shape.horizontalRadiusFactor, walk);
        const double horizontal = baseHorizontal * static_cast<double>(horizontalFactor);
        const float fraction = static_cast<float>(i) / static_cast<float>(length);
        const float centred = std::fabs(0.5F - fraction) * 2.0F;
        const float centre = 1.0F - centred;
        const float multiplier =
            shape.verticalRadiusDefaultFactor + (shape.verticalRadiusCenterFactor * centre);
        const float verticalDraw = draw(walk) * 0.25F;
        const float verticalFactor = verticalDraw + 0.75F;
        const double vertical =
            (static_cast<double>(multiplier) * baseVertical) * static_cast<double>(verticalFactor);

        // Position.
        const float cosPitch = tableCos(static_cast<double>(phi));
        const float sinPitch = tableSin(static_cast<double>(phi));
        x += static_cast<double>(tableCos(static_cast<double>(theta)) * cosPitch);
        y += static_cast<double>(sinPitch);
        z += static_cast<double>(tableSin(static_cast<double>(theta)) * cosPitch);

        // Pitch, heading, velocities: no steep flag (B-Q3.2).
        const float damped = phi * 0.7F;
        phi = damped + (pitchVelocity * 0.05F);
        theta = theta + (headingVelocity * 0.05F);
        pitchVelocity = pitchVelocity * 0.8F;
        headingVelocity = headingVelocity * 0.5F;
        {
            const float a = draw(walk);
            const float b = draw(walk);
            const float c = draw(walk);
            pitchVelocity = pitchVelocity + (((a - b) * c) * 2.0F);
        }
        {
            const float a = draw(walk);
            const float b = draw(walk);
            const float c = draw(walk);
            headingVelocity = headingVelocity + (((a - b) * c) * 4.0F);
        }

        if (walk.nextInt(4) == 0) {
            continue;
        }
        if (!carving.reaches(x, z, length, i, thickness)) {
            return;
        }
        carving.ellipsoid(carver, x, y, z, horizontal, vertical, 0.0, false, &widths);
    }
}

} // namespace

void carveChunk(const CarverSet& carvers, const CarveTarget& target) {
    if (carvers.empty()) {
        return;
    }
    Carving carving(target);
    for (std::int32_t dx = -kWindow; dx <= kWindow; ++dx) {
        for (std::int32_t dz = -kWindow; dz <= kWindow; ++dz) {
            const std::int32_t sx = target.chunkX + dx;
            const std::int32_t sz = target.chunkZ + dz;
            // A-Q11.2: the start chunk's biome, at quart (4 Sx, 0, 4 Sz).
            const data::ResourceLocation& biome = target.biomeAt(sx * 16, sz * 16);
            const auto& list = carvers.carversOf(biome);
            for (std::size_t k = 0; k < list.size(); ++k) {
                const CompiledCarver& carver = *list[k];
                // A-Q1.1: seed W + k, two longs, then re-seed with
                // (Sx A) ^ (Sz B) ^ (W + k), all 64-bit wrapping.
                const std::int64_t base =
                    javamath::wrappingAdd(target.worldSeed, static_cast<std::int64_t>(k));
                JavaRandom g{base};
                const std::int64_t a = g.nextLong();
                const std::int64_t b = g.nextLong();
                const std::int64_t mixed = javamath::wrappingMul(static_cast<std::int64_t>(sx), a) ^
                                           javamath::wrappingMul(static_cast<std::int64_t>(sz), b) ^
                                           base;
                g.setSeed(mixed);
                // A-Q1.3: the start test, single precision, non-strict.
                if (!(g.nextFloat() <= carver.config.probability)) {
                    continue;
                }
                if (carver.config.type == CarverType::Canyon) {
                    canyonStart(carving, carver, g, sx, sz, target.geometry);
                } else {
                    caveStart(carving, carver, g, sx, sz, target.geometry);
                }
            }
        }
    }
}

namespace {

[[nodiscard]] std::shared_ptr<const CompiledCarver> compile(ConfiguredCarver config,
                                                            const CarverSources& sources) {
    auto compiled = std::make_shared<CompiledCarver>();
    if (config.replaceable.isTag) {
        const auto found = sources.tags.find(config.replaceable.tag);
        if (found == sources.tags.end()) {
            throw CarverError("replaceable tag #" + config.replaceable.tag.toString() +
                              " is not among the resolved carver tags");
        }
        compiled->replaceable = found->second;
    } else {
        compiled->replaceable.insert(config.replaceable.blocks.begin(),
                                     config.replaceable.blocks.end());
    }
    compiled->config = std::move(config);
    return compiled;
}

void noteTag(const ConfiguredCarver& carver, const data::BlockTags& tags, CarverSources& into) {
    if (carver.replaceable.isTag && !into.tags.contains(carver.replaceable.tag)) {
        into.tags.emplace(carver.replaceable.tag, tags.resolve(carver.replaceable.tag));
    }
}

} // namespace

CarverSources gatherCarverSources(const data::Pack& pack, const data::BlockTags& tags) {
    CarverSources sources;
    for (const data::PackEntry* entry : pack.entriesOf(data::Registry::ConfiguredCarver)) {
        noteTag(ConfiguredCarver::fromJson(entry->json, entry->id.toString()), tags, sources);
        sources.configured.emplace(entry->id, entry->json);
    }
    for (const data::PackEntry* entry : pack.entriesOf(data::Registry::Biome)) {
        if (!entry->json.contains("carvers")) {
            throw CarverError("biome " + entry->id.toString() + " has no carvers field");
        }
        const nlohmann::json& field = entry->json.at("carvers");
        for (const CarverEntry& carver : parseBiomeCarvers(field, entry->id.toString())) {
            if (const auto* defined = std::get_if<ConfiguredCarver>(&carver)) {
                noteTag(*defined, tags, sources);
            }
        }
        sources.biomeCarvers.emplace(entry->id, field);
    }
    // Every reference resolves, checked now rather than at the first chunk.
    static_cast<void>(buildCarverSet(sources));
    return sources;
}

CarverSet buildCarverSet(const CarverSources& sources) {
    std::map<data::ResourceLocation, std::shared_ptr<const CompiledCarver>> configured;
    for (const auto& [id, json] : sources.configured) {
        configured.emplace(id, compile(ConfiguredCarver::fromJson(json, id.toString()), sources));
    }
    CarverSet set;
    for (const auto& [biome, field] : sources.biomeCarvers) {
        std::vector<std::shared_ptr<const CompiledCarver>> list;
        for (CarverEntry& entry : parseBiomeCarvers(field, biome.toString())) {
            if (auto* id = std::get_if<data::ResourceLocation>(&entry)) {
                const auto found = configured.find(*id);
                if (found == configured.end()) {
                    throw CarverError("biome " + biome.toString() + " names configured carver " +
                                      id->toString() + ", which the pack does not define");
                }
                list.push_back(found->second);
            } else {
                list.push_back(compile(std::get<ConfiguredCarver>(std::move(entry)), sources));
            }
        }
        set.setBiome(biome, std::move(list));
    }
    return set;
}

} // namespace stratum::carver
