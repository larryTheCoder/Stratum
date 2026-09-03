// Stratum — the per-world pipeline freeze.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0

#include <stratum/data/resource_location.hpp>
#include <stratum/density/graph.hpp>
#include <stratum/density/noise_registry.hpp>
#include <stratum/freeze/pipeline.hpp>
#include <stratum/hash/md5.hpp>
#include <stratum/settings/noise_settings.hpp>
#include <stratum/version.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace stratum::freeze {

namespace {

/// "STRTMPIP". Present so that a file which is not a blob at all is refused
/// as such rather than as a malformed one.
constexpr std::array<std::byte, 8> kMagic = {std::byte{'S'}, std::byte{'T'}, std::byte{'R'},
                                             std::byte{'T'}, std::byte{'M'}, std::byte{'P'},
                                             std::byte{'I'}, std::byte{'P'}};

/// Which spelling a node's `noise` field used. Written as a tag so that the
/// two the schema allows — an identifier, or the parameters inline — are
/// told apart on the way back in.
constexpr std::uint8_t kNoiseAbsent = 0;
constexpr std::uint8_t kNoiseNamed = 1;
constexpr std::uint8_t kNoiseInline = 2;

/// A blob with more than this in any one collection is not a pipeline, it is
/// a corrupt length being believed. Bounds are checked before allocation so
/// that a wrong number cannot ask for a terabyte.
constexpr std::uint64_t kSaneCount = 10'000'000;

class Writer {
public:
    void u8(std::uint8_t value) { bytes_.push_back(static_cast<std::byte>(value)); }

    /// Little-endian with an explicit width, everywhere. The point is that a
    /// blob written on one machine reads the same on another, and "whatever
    /// the host does" is not that.
    void u32(std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            u8(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
        }
    }

    void u64(std::uint64_t value) {
        for (int shift = 0; shift < 64; shift += 8) {
            u8(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
        }
    }

    void i32(std::int32_t value) { u32(static_cast<std::uint32_t>(value)); }

    /// The bit pattern, not a rendering. A double printed and reparsed is a
    /// platform's opinion about formatting; parity needs the bits.
    void f64(double value) { u64(std::bit_cast<std::uint64_t>(value)); }

    void str(std::string_view text) {
        u32(static_cast<std::uint32_t>(text.size()));
        for (const char character : text) {
            u8(static_cast<std::uint8_t>(character));
        }
    }

    void id(const data::ResourceLocation& value) { str(value.toString()); }

    void bytes(std::span<const std::byte> data) {
        bytes_.insert(bytes_.end(), data.begin(), data.end());
    }

    [[nodiscard]] const std::vector<std::byte>& take() const noexcept { return bytes_; }

private:
    std::vector<std::byte> bytes_;
};

class Reader {
public:
    explicit Reader(std::span<const std::byte> data) : data_(data) {}

    [[nodiscard]] std::uint8_t u8() {
        if (at_ >= data_.size()) {
            fail("ran off the end");
        }
        return static_cast<std::uint8_t>(data_[at_++]);
    }

    [[nodiscard]] std::uint32_t u32() {
        std::uint32_t value = 0;
        for (int shift = 0; shift < 32; shift += 8) {
            value |= static_cast<std::uint32_t>(u8()) << shift;
        }
        return value;
    }

    [[nodiscard]] std::uint64_t u64() {
        std::uint64_t value = 0;
        for (int shift = 0; shift < 64; shift += 8) {
            value |= static_cast<std::uint64_t>(u8()) << shift;
        }
        return value;
    }

    [[nodiscard]] std::int32_t i32() { return static_cast<std::int32_t>(u32()); }

    [[nodiscard]] double f64() { return std::bit_cast<double>(u64()); }

    [[nodiscard]] std::string str() {
        const std::uint32_t length = u32();
        if (length > data_.size() - at_) {
            fail("a string claims " + std::to_string(length) + " bytes and the blob has " +
                 std::to_string(data_.size() - at_) + " left");
        }
        std::string text;
        text.reserve(length);
        for (std::uint32_t i = 0; i < length; ++i) {
            text.push_back(static_cast<char>(u8()));
        }
        return text;
    }

    [[nodiscard]] data::ResourceLocation id() {
        try {
            return data::ResourceLocation::parse(str());
        } catch (const data::ResourceLocationError& error) {
            fail(error.what());
        }
    }

    /// Every count is bounded by the blob's own remaining size before it is
    /// used to reserve anything. An absolute ceiling is not enough on its
    /// own: a corrupt file can hold a dozen counts, each individually
    /// plausible, that together ask for more memory than the machine has.
    /// Every entry costs at least one byte, so the bytes left is the honest
    /// bound.
    [[nodiscard]] std::uint32_t count(const char* what) {
        const std::uint32_t value = u32();
        const std::size_t remaining = data_.size() - at_;
        if (value > kSaneCount || value > remaining) {
            fail(std::string(what) + " claims " + std::to_string(value) +
                 " entries and there are " + std::to_string(remaining) +
                 " bytes left; this is not a pipeline");
        }
        return value;
    }

    [[noreturn]] void fail(const std::string& what) const {
        throw FreezeError("frozen pipeline is malformed at byte " + std::to_string(at_) + ": " +
                          what);
    }

    [[nodiscard]] std::size_t at() const noexcept { return at_; }

    [[nodiscard]] bool exhausted() const noexcept { return at_ == data_.size(); }

private:
    std::span<const std::byte> data_;
    std::size_t at_ = 0;
};

void writeNoises(Writer& out,
                 const std::map<data::ResourceLocation, density::NoiseParameters>& noises) {
    out.u32(static_cast<std::uint32_t>(noises.size()));
    // std::map iterates in key order, which is what makes this reproducible
    // without sorting anything here.
    for (const auto& [id, parameters] : noises) {
        out.id(id);
        out.i32(parameters.firstOctave);
        out.u32(static_cast<std::uint32_t>(parameters.amplitudes.size()));
        for (const double amplitude : parameters.amplitudes) {
            out.f64(amplitude);
        }
    }
}

/// A node's `noise` field: a tag saying which of the schema's two spellings
/// it used, then that spelling. A tag rather than a present/absent flag
/// because losing the distinction would turn an inline noise into a node
/// that samples nothing.
///
/// Its own function rather than three branches inside writeGraph's loop, and
/// that is not only tidiness: clang-tidy's optional-access analysis gives up
/// on a body this size, and once it does it reports the *spline* field's
/// perfectly good guard as unchecked.
void writeNodeNoise(Writer& out, const density::Node& node) {
    if (node.noise.has_value()) {
        out.u8(kNoiseNamed);
        out.id(*node.noise);
        return;
    }
    if (!node.inlineNoise.has_value()) {
        out.u8(kNoiseAbsent);
        return;
    }
    out.u8(kNoiseInline);
    out.i32(node.inlineNoise->firstOctave);
    out.u32(static_cast<std::uint32_t>(node.inlineNoise->amplitudes.size()));
    for (const double amplitude : node.inlineNoise->amplitudes) {
        out.f64(amplitude);
    }
}

void writeGraph(Writer& out, const density::Graph& graph) {
    out.u32(static_cast<std::uint32_t>(graph.nodeCount()));
    for (std::size_t i = 0; i < graph.nodeCount(); ++i) {
        const density::Node& node = graph.node(static_cast<density::NodeIndex>(i));
        out.u8(static_cast<std::uint8_t>(node.type));
        out.u32(static_cast<std::uint32_t>(node.arguments.size()));
        for (const density::NodeIndex argument : node.arguments) {
            out.u32(argument);
        }
        out.u32(static_cast<std::uint32_t>(node.parameters.size()));
        for (const double parameter : node.parameters) {
            out.f64(parameter);
        }
        writeNodeNoise(out, node);
        out.u8(node.spline.has_value() ? 1U : 0U);
        if (node.spline.has_value()) {
            out.u32(*node.spline);
        }
        out.str(node.selector);
    }

    out.u32(static_cast<std::uint32_t>(graph.splineCount()));
    for (std::size_t i = 0; i < graph.splineCount(); ++i) {
        const density::SplineDefinition& spline =
            graph.spline(static_cast<density::SplineIndex>(i));
        out.u32(spline.coordinate);
        out.u32(static_cast<std::uint32_t>(spline.points.size()));
        for (const density::SplinePoint& point : spline.points) {
            out.f64(point.location);
            out.f64(point.derivative);
            out.u8(point.value.has_value() ? 1U : 0U);
            if (point.value.has_value()) {
                out.f64(*point.value);
            } else if (point.nested.has_value()) {
                out.u32(*point.nested);
            } else {
                // The resolver never builds a point that is neither, and the
                // assembler refuses one that is; this layer still does not
                // get to assume its input came from either of them.
                throw FreezeError("a spline point is neither a value nor a nested spline, so "
                                  "this pipeline cannot be frozen");
            }
        }
    }

    out.u32(static_cast<std::uint32_t>(graph.roots().size()));
    for (const auto& [id, root] : graph.roots()) {
        out.id(id);
        out.u32(root);
    }
}

void writeBlockState(Writer& out, const settings::BlockState& state) {
    out.id(state.name);
    out.u32(static_cast<std::uint32_t>(state.properties.size()));
    for (const auto& [key, value] : state.properties) {
        out.str(key);
        out.str(value);
    }
}

void writeSettings(Writer& out,
                   const std::map<data::ResourceLocation, settings::NoiseSettings>& all) {
    out.u32(static_cast<std::uint32_t>(all.size()));
    for (const auto& [id, one] : all) {
        out.id(id);
        writeBlockState(out, one.defaultBlock);
        writeBlockState(out, one.defaultFluid);
        out.i32(one.seaLevel);
        out.u8(one.disableMobGeneration ? 1U : 0U);
        out.u8(one.aquifersEnabled ? 1U : 0U);
        out.u8(one.oreVeinsEnabled ? 1U : 0U);
        out.u8(one.legacyRandomSource ? 1U : 0U);
        out.i32(one.geometry.minY);
        out.i32(one.geometry.height);
        out.i32(one.geometry.sizeHorizontal);
        out.i32(one.geometry.sizeVertical);
        for (const density::NodeIndex entry : one.router.entries) {
            out.u32(entry);
        }
        // Kept as text because this build does not interpret them (M4), and
        // a pipeline has to round-trip what it does not understand as
        // faithfully as what it does. nlohmann orders object keys, so the
        // dump is stable.
        out.str(one.surfaceRule.dump());
        out.str(one.spawnTarget.dump());
    }
}

[[nodiscard]] std::map<data::ResourceLocation, density::NoiseParameters> readNoises(Reader& in) {
    std::map<data::ResourceLocation, density::NoiseParameters> noises;
    const std::uint32_t entries = in.count("the noise table");
    for (std::uint32_t i = 0; i < entries; ++i) {
        const data::ResourceLocation id = in.id();
        density::NoiseParameters parameters;
        parameters.firstOctave = in.i32();
        const std::uint32_t amplitudes = in.count("a noise's amplitudes");
        parameters.amplitudes.reserve(amplitudes);
        for (std::uint32_t k = 0; k < amplitudes; ++k) {
            parameters.amplitudes.push_back(in.f64());
        }
        noises.emplace(id, std::move(parameters));
    }
    return noises;
}

[[nodiscard]] density::Graph readGraph(Reader& in) {
    density::Graph::Assembler assembler;

    const std::uint32_t nodes = in.count("the node table");
    std::vector<density::Node> pending;
    pending.reserve(nodes);
    for (std::uint32_t i = 0; i < nodes; ++i) {
        density::Node node;
        const std::uint8_t type = in.u8();
        if (density::nodeTypeName(static_cast<density::NodeType>(type)) == "unknown") {
            in.fail("node " + std::to_string(i) + " has type " + std::to_string(type) +
                    ", which this build does not know");
        }
        node.type = static_cast<density::NodeType>(type);
        const std::uint32_t arguments = in.count("a node's arguments");
        node.arguments.reserve(arguments);
        for (std::uint32_t k = 0; k < arguments; ++k) {
            node.arguments.push_back(in.u32());
        }
        const std::uint32_t parameters = in.count("a node's parameters");
        node.parameters.reserve(parameters);
        for (std::uint32_t k = 0; k < parameters; ++k) {
            node.parameters.push_back(in.f64());
        }
        switch (const std::uint8_t noiseTag = in.u8(); noiseTag) {
            case kNoiseAbsent:
                break;
            case kNoiseNamed:
                node.noise = in.id();
                break;
            case kNoiseInline: {
                density::NoiseParameters inlineNoise;
                inlineNoise.firstOctave = in.i32();
                const std::uint32_t amplitudes = in.count("an inline noise's amplitudes");
                inlineNoise.amplitudes.reserve(amplitudes);
                for (std::uint32_t k = 0; k < amplitudes; ++k) {
                    inlineNoise.amplitudes.push_back(in.f64());
                }
                node.inlineNoise = std::move(inlineNoise);
                break;
            }
            default:
                in.fail("node " + std::to_string(i) + " tags its noise field " +
                        std::to_string(noiseTag) + ", which is not one of the two spellings");
        }
        if (in.u8() != 0U) {
            node.spline = in.u32();
        }
        node.selector = in.str();
        pending.push_back(std::move(node));
    }

    // Splines are read before the nodes are added, because a node may name a
    // spline and a spline names a node. The resolver produced them in an
    // order where that is consistent; the assembler checks it still is.
    const std::uint32_t splines = in.count("the spline table");
    std::vector<density::SplineDefinition> pendingSplines;
    pendingSplines.reserve(splines);
    for (std::uint32_t i = 0; i < splines; ++i) {
        density::SplineDefinition spline;
        spline.coordinate = in.u32();
        const std::uint32_t points = in.count("a spline's points");
        spline.points.reserve(points);
        for (std::uint32_t k = 0; k < points; ++k) {
            density::SplinePoint point;
            point.location = in.f64();
            point.derivative = in.f64();
            if (in.u8() != 0U) {
                point.value = in.f64();
            } else {
                point.nested = in.u32();
            }
            spline.points.push_back(point);
        }
        pendingSplines.push_back(std::move(spline));
    }

    try {
        for (density::Node& node : pending) {
            static_cast<void>(assembler.addNode(std::move(node)));
        }
        for (density::SplineDefinition& spline : pendingSplines) {
            static_cast<void>(assembler.addSpline(std::move(spline)));
        }
    } catch (const density::ResolveError& error) {
        in.fail(error.what());
    }

    const std::uint32_t roots = in.count("the root table");
    // Roots are added before release() so that a root naming a node past the
    // end is caught by the assembler rather than by an out-of-range read
    // later.
    for (std::uint32_t i = 0; i < roots; ++i) {
        const data::ResourceLocation id = in.id();
        const density::NodeIndex root = in.u32();
        try {
            assembler.addRoot(id, root);
        } catch (const density::ResolveError& error) {
            in.fail(error.what());
        }
    }

    // release() is where the node/spline cross-references are checked, so it
    // throws as readily as the adds do and has to be wrapped with them.
    try {
        return assembler.release();
    } catch (const density::ResolveError& error) {
        in.fail(error.what());
    }
}

[[nodiscard]] settings::BlockState readBlockState(Reader& in) {
    settings::BlockState state;
    state.name = in.id();
    const std::uint32_t properties = in.count("a block state's properties");
    for (std::uint32_t i = 0; i < properties; ++i) {
        const std::string key = in.str();
        state.properties.emplace(key, in.str());
    }
    return state;
}

[[nodiscard]] std::map<data::ResourceLocation, settings::NoiseSettings> readSettings(Reader& in) {
    std::map<data::ResourceLocation, settings::NoiseSettings> all;
    const std::uint32_t entries = in.count("the noise settings table");
    for (std::uint32_t i = 0; i < entries; ++i) {
        settings::NoiseSettings one;
        one.id = in.id();
        one.defaultBlock = readBlockState(in);
        one.defaultFluid = readBlockState(in);
        one.seaLevel = in.i32();
        one.disableMobGeneration = in.u8() != 0U;
        one.aquifersEnabled = in.u8() != 0U;
        one.oreVeinsEnabled = in.u8() != 0U;
        one.legacyRandomSource = in.u8() != 0U;
        one.geometry.minY = in.i32();
        one.geometry.height = in.i32();
        one.geometry.sizeHorizontal = in.i32();
        one.geometry.sizeVertical = in.i32();
        for (density::NodeIndex& entry : one.router.entries) {
            entry = in.u32();
        }
        const std::string surfaceRule = in.str();
        const std::string spawnTarget = in.str();
        try {
            one.surfaceRule = nlohmann::json::parse(surfaceRule);
            one.spawnTarget = nlohmann::json::parse(spawnTarget);
        } catch (const nlohmann::json::exception& error) {
            in.fail(std::string("a stored surface rule or spawn target is not JSON: ") +
                    error.what());
        }
        const data::ResourceLocation id = one.id;
        all.emplace(id, std::move(one));
    }
    return all;
}

/// The payload, without the header. Split out because the hash is over
/// exactly this and nothing else.
[[nodiscard]] std::vector<std::byte> writePayload(const Pipeline& pipeline) {
    Writer out;
    writeNoises(out, pipeline.noises);
    writeGraph(out, pipeline.graph);
    writeSettings(out, pipeline.settings);
    return out.take();
}

constexpr std::size_t kHeaderFixedSize = kMagic.size() + 4 + 4;

} // namespace

std::vector<std::byte> write(const Pipeline& pipeline) {
    const std::vector<std::byte> payload = writePayload(pipeline);
    const hash::Md5Digest digest = hash::md5(payload);

    Writer out;
    out.bytes(kMagic);
    out.u32(kBlobFormat);
    out.u32(kPipelineEngineVersion);
    out.str(kMinecraftVersion);
    out.i32(kPackFormatMajor);
    out.i32(kPackFormatMinor);
    for (const std::uint8_t byte : digest) {
        out.u8(byte);
    }
    out.u64(static_cast<std::uint64_t>(payload.size()));
    out.bytes(payload);
    return out.take();
}

Provenance inspect(std::span<const std::byte> blob) {
    if (blob.size() < kHeaderFixedSize) {
        throw FreezeError("not a frozen pipeline: only " + std::to_string(blob.size()) +
                          " bytes, too few even for a header");
    }
    if (!std::equal(kMagic.begin(), kMagic.end(), blob.begin())) {
        throw FreezeError("not a frozen pipeline: the file does not begin with STRTMPIP");
    }

    Reader in(blob);
    for (std::size_t i = 0; i < kMagic.size(); ++i) {
        static_cast<void>(in.u8());
    }

    Provenance provenance;
    provenance.blobFormat = in.u32();
    provenance.pipelineEngineVersion = in.u32();
    provenance.minecraftVersion = in.str();
    provenance.packFormatMajor = in.i32();
    provenance.packFormatMinor = in.i32();
    for (std::uint8_t& byte : provenance.contentHash) {
        byte = in.u8();
    }
    return provenance;
}

Pipeline read(std::span<const std::byte> blob) {
    const Provenance provenance = inspect(blob);

    if (provenance.blobFormat != kBlobFormat) {
        throw FreezeError("this world's pipeline is stored in container format " +
                          std::to_string(provenance.blobFormat) + " and this build writes " +
                          std::to_string(kBlobFormat) + "; it cannot be read");
    }
    // The load-bearing refusal (SPEC §6). A build that generates differently
    // from the one that froze this must not open the world at all: opening it
    // and generating anyway is how a seam gets into a world that already has
    // terrain in it.
    if (provenance.pipelineEngineVersion != kPipelineEngineVersion) {
        throw FreezeError(
            "this world was frozen by pipeline engine version " +
            std::to_string(provenance.pipelineEngineVersion) + " and this build is version " +
            std::to_string(kPipelineEngineVersion) +
            ", which is not guaranteed to generate the same terrain. Refusing to generate for "
            "this world rather than risk a seam in it (SPEC §6). Use an engine build whose "
            "pipeline engine version is " +
            std::to_string(provenance.pipelineEngineVersion) + ".");
    }
    if (provenance.minecraftVersion != kMinecraftVersion) {
        throw FreezeError("this world was frozen against the Minecraft " +
                          provenance.minecraftVersion + " schema and this build is pinned to " +
                          std::string(kMinecraftVersion));
    }

    // Re-read past the header to reach the payload.
    Reader in(blob);
    for (std::size_t i = 0; i < kMagic.size(); ++i) {
        static_cast<void>(in.u8());
    }
    static_cast<void>(in.u32());
    static_cast<void>(in.u32());
    static_cast<void>(in.str());
    static_cast<void>(in.i32());
    static_cast<void>(in.i32());
    for (std::size_t i = 0; i < provenance.contentHash.size(); ++i) {
        static_cast<void>(in.u8());
    }
    const std::uint64_t payloadSize = in.u64();
    const std::size_t payloadAt = in.at();
    if (payloadSize != blob.size() - payloadAt) {
        throw FreezeError("this world's pipeline claims a payload of " +
                          std::to_string(payloadSize) + " bytes and the file holds " +
                          std::to_string(blob.size() - payloadAt) +
                          "; it is truncated or has "
                          "something appended");
    }

    const std::span<const std::byte> payload = blob.subspan(payloadAt);
    // Checked before anything in it is believed. MD5 is an integrity check
    // here and not a security boundary: the question is whether the file
    // changed, not whether someone forged it.
    if (hash::md5(payload) != provenance.contentHash) {
        throw FreezeError("this world's pipeline does not match its own content hash; the file "
                          "has been corrupted or edited, and generating from it would produce "
                          "terrain unlike the rest of the world");
    }

    Reader payloadReader(payload);
    Pipeline pipeline;
    pipeline.noises = readNoises(payloadReader);
    pipeline.graph = readGraph(payloadReader);
    pipeline.settings = readSettings(payloadReader);
    if (!payloadReader.exhausted()) {
        throw FreezeError("this world's pipeline has trailing bytes after the settings, so it "
                          "is not the shape this build writes");
    }
    return pipeline;
}

} // namespace stratum::freeze
