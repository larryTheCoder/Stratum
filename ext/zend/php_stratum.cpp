// Stratum — the zend module PocketMine-MP loads.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Marshaling only (SPEC §4.3): every function here parses PHP arguments,
// calls into stratum_pmmp_core / lib, and turns the answer — or the C++
// exception — into PHP values. No generation logic lives in this file.
//
// PHP surface (namespace Stratum; stratum.stub.php documents the shapes):
//
//   function freezePipeline(string $versionRoot, string $blobPath): void
//   function bedrockBlockState(int $javaStateId): array
//   function javaBlockStateCount(): int
//   final class Dimension {
//       static function open(string $blobPath, string $noiseSettings,
//                            string $biomeParameterList, int $seed): Dimension
//       function getMinY(): int
//       function getHeight(): int
//       function encodeChunk(int $chunkX, int $chunkZ): array
//   }
//   final class GenerationException extends \RuntimeException {}
//
// THREADS. PocketMine-MP builds one Generator per worker thread, each in its
// own PHP request. A compiled dimension is immutable (SPEC §4.1), so every
// thread opening the same world shares one: `Registry::open` keeps a
// process-wide registry of weak references, locked only while looking up or
// compiling, never while generating. PHP objects themselves never cross
// threads; each thread's Dimension object holds its own shared_ptr.
//
// EXCEPTIONS never cross into the engine: a C++ exception unwinding through
// zend frames is undefined behaviour, so every entry point catches and
// rethrows as Stratum\GenerationException.

#include <stratum/data/pack.hpp>
#include <stratum/data/resource_location.hpp>
#include <stratum/freeze/pipeline.hpp>
#include <stratum/mapping/block_state.hpp>
#include <stratum/world/dimension.hpp>

#include <stratum_pmmp/chunk_encoder.hpp>

// php.h has to come first — the others use macros it defines — and
// clang-format would sort it last.
// clang-format off
extern "C" {
#include <php.h>
#include <Zend/zend_exceptions.h>
#include <ext/spl/spl_exceptions.h>
#include <ext/standard/info.h>
}
// clang-format on

#include "php_stratum.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <tuple>
#include <vector>

namespace {

zend_class_entry* generationExceptionEntry =
    nullptr; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
zend_class_entry* dimensionEntry =
    nullptr; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
zend_object_handlers
    dimensionHandlers; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

// --- the shared registry --------------------------------------------------

using RegistryKey = std::tuple<std::string, std::string, std::string, std::int64_t>;

class Registry {
public:
    std::shared_ptr<const stratum::world::CompiledDimension>
    open(const std::string& blobPath, const std::string& noiseSettings,
         const std::string& biomeParameterList, const std::int64_t seed) {
        std::ifstream file(blobPath, std::ios::binary);
        if (!file) {
            throw std::runtime_error("cannot read the frozen pipeline at " + blobPath);
        }
        const std::vector<char> raw((std::istreambuf_iterator<char>(file)),
                                    std::istreambuf_iterator<char>());
        std::vector<std::byte> blob(raw.size());
        std::memcpy(blob.data(), raw.data(), raw.size());

        // Keyed by the blob's own content hash, not its path: two paths to
        // one pipeline share, and an edited file at the same path does not.
        const auto provenance = stratum::freeze::inspect(blob);
        std::string hash(provenance.contentHash.size() * 2, '0');
        for (std::size_t i = 0; i < provenance.contentHash.size(); ++i) {
            static constexpr std::array<char, 16> kHex = {'0', '1', '2', '3', '4', '5', '6', '7',
                                                          '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
            hash[2 * i] = kHex.at(provenance.contentHash.at(i) >> 4U);
            hash[(2 * i) + 1] = kHex.at(provenance.contentHash.at(i) & 0x0FU);
        }
        const RegistryKey key{hash, noiseSettings, biomeParameterList, seed};

        const std::lock_guard<std::mutex> lock(mutex_);
        if (auto existing = dimensions_[key].lock()) {
            return existing;
        }
        std::shared_ptr<const stratum::world::CompiledDimension> compiled =
            stratum::world::CompiledDimension::compile(
                stratum::freeze::read(blob), stratum::data::ResourceLocation::parse(noiseSettings),
                stratum::data::ResourceLocation::parse(biomeParameterList), seed);
        dimensions_[key] = compiled;
        return compiled;
    }

private:
    std::mutex mutex_;
    std::map<RegistryKey, std::weak_ptr<const stratum::world::CompiledDimension>> dimensions_;
};

Registry& registry() {
    static Registry instance;
    return instance;
}

// --- errors -----------------------------------------------------------------

/// Rethrows the in-flight C++ exception as Stratum\GenerationException.
/// Called from a catch block only.
void throwAsPhp() {
    try {
        throw;
    } catch (const std::exception& error) {
        zend_throw_exception(generationExceptionEntry, error.what(), 0);
    } catch (...) {
        zend_throw_exception(generationExceptionEntry, "unknown error inside Stratum", 0);
    }
}

// --- Dimension objects --------------------------------------------------------

struct DimensionObject {
    std::shared_ptr<const stratum::world::CompiledDimension> dimension;
    zend_object std;
};

DimensionObject* fetchDimension(zend_object* object) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return reinterpret_cast<DimensionObject*>(reinterpret_cast<char*>(object) -
                                              XtOffsetOf(DimensionObject, std));
}

zend_object* dimensionCreate(zend_class_entry* classType) {
    auto* object = static_cast<DimensionObject*>(
        ecalloc(1, sizeof(DimensionObject) + zend_object_properties_size(classType)));
    new (&object->dimension) std::shared_ptr<const stratum::world::CompiledDimension>();
    zend_object_std_init(&object->std, classType);
    object_properties_init(&object->std, classType);
    object->std.handlers = &dimensionHandlers;
    return &object->std;
}

void dimensionFree(zend_object* object) {
    DimensionObject* intern = fetchDimension(object);
    intern->dimension.~shared_ptr();
    zend_object_std_dtor(&intern->std);
}

void addLayer(zval* into, const char* key, const stratum::pmmp::PalettedLayer& layer) {
    zval triple;
    array_init_size(&triple, 3);
    add_next_index_long(&triple, layer.bitsPerBlock);
    // Host-endian bytes, exactly as PalettedBlockArray::fromData memcpy()s them.
    add_next_index_stringl(&triple,
                           reinterpret_cast<const char*>(layer.words.data()), // NOLINT
                           layer.words.size() * sizeof(std::uint32_t));
    zval palette;
    array_init_size(&palette, static_cast<std::uint32_t>(layer.palette.size()));
    for (const std::uint32_t value : layer.palette) {
        add_next_index_long(&palette, static_cast<zend_long>(value));
    }
    add_next_index_zval(&triple, &palette);
    add_assoc_zval(into, key, &triple);
}

} // namespace

extern "C" {

// NOLINTBEGIN(readability-identifier-naming,cppcoreguidelines-*,modernize-*,hicpp-*)

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_freezePipeline, 0, 2, IS_VOID, 0)
ZEND_ARG_TYPE_INFO(0, versionRoot, IS_STRING, 0)
ZEND_ARG_TYPE_INFO(0, blobPath, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bedrockBlockState, 0, 1, IS_ARRAY, 0)
ZEND_ARG_TYPE_INFO(0, javaStateId, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_javaBlockStateCount, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_Dimension___construct, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_Dimension_open, 0, 4, Stratum\\Dimension, 0)
ZEND_ARG_TYPE_INFO(0, blobPath, IS_STRING, 0)
ZEND_ARG_TYPE_INFO(0, noiseSettings, IS_STRING, 0)
ZEND_ARG_TYPE_INFO(0, biomeParameterList, IS_STRING, 0)
ZEND_ARG_TYPE_INFO(0, seed, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_Dimension_getInt, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_Dimension_encodeChunk, 0, 2, IS_ARRAY, 0)
ZEND_ARG_TYPE_INFO(0, chunkX, IS_LONG, 0)
ZEND_ARG_TYPE_INFO(0, chunkZ, IS_LONG, 0)
ZEND_END_ARG_INFO()

static ZEND_FUNCTION(stratum_freezePipeline) {
    zend_string* versionRoot = nullptr;
    zend_string* blobPath = nullptr;
    ZEND_PARSE_PARAMETERS_START(2, 2)
    Z_PARAM_STR(versionRoot)
    Z_PARAM_STR(blobPath)
    ZEND_PARSE_PARAMETERS_END();
    try {
        const std::filesystem::path root(std::string(ZSTR_VAL(versionRoot), ZSTR_LEN(versionRoot)));
        const std::vector<std::byte> blob = stratum::freeze::write(stratum::freeze::resolve(
            stratum::data::Pack::open(root / "worldgen"), root / "biome_parameters"));
        const std::string target(ZSTR_VAL(blobPath), ZSTR_LEN(blobPath));
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(blob.data()),
                  static_cast<std::streamsize>(blob.size()));
        if (!out) {
            throw std::runtime_error("cannot write the frozen pipeline to " + target);
        }
    } catch (...) {
        throwAsPhp();
    }
}

static ZEND_FUNCTION(stratum_bedrockBlockState) {
    zend_long javaStateId = 0;
    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_LONG(javaStateId)
    ZEND_PARSE_PARAMETERS_END();
    try {
        if (javaStateId < 0) {
            throw std::runtime_error("Java block state id " + std::to_string(javaStateId) +
                                     " is negative");
        }
        const stratum::mapping::BedrockBlockState state =
            stratum::mapping::bedrockBlockState(static_cast<std::uint32_t>(javaStateId));
        array_init_size(return_value, 3);
        add_assoc_stringl(return_value, "name", state.name.data(), state.name.size());
        zval states;
        array_init_size(&states, static_cast<std::uint32_t>(state.states.size()));
        for (const stratum::mapping::BedrockStateValue& value : state.states) {
            zval pair;
            array_init_size(&pair, 2);
            // The NBT tag id, so PHP can build ByteTag / IntTag / StringTag.
            add_next_index_long(&pair, static_cast<zend_long>(value.type));
            if (value.type == stratum::mapping::BedrockTagType::String) {
                add_next_index_stringl(&pair, value.string.data(), value.string.size());
            } else {
                add_next_index_long(&pair, value.integer);
            }
            add_assoc_zval_ex(&states, value.name.data(), value.name.size(), &pair);
        }
        add_assoc_zval(return_value, "states", &states);
        add_assoc_long(return_value, "version", state.version);
    } catch (...) {
        throwAsPhp();
    }
}

static ZEND_FUNCTION(stratum_javaBlockStateCount) {
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_LONG(static_cast<zend_long>(stratum::mapping::javaBlockStateCount()));
}

static ZEND_METHOD(Stratum_Dimension, __construct) {
    ZEND_PARSE_PARAMETERS_NONE();
}

static ZEND_METHOD(Stratum_Dimension, open) {
    zend_string* blobPath = nullptr;
    zend_string* noiseSettings = nullptr;
    zend_string* biomeParameterList = nullptr;
    zend_long seed = 0;
    ZEND_PARSE_PARAMETERS_START(4, 4)
    Z_PARAM_STR(blobPath)
    Z_PARAM_STR(noiseSettings)
    Z_PARAM_STR(biomeParameterList)
    Z_PARAM_LONG(seed)
    ZEND_PARSE_PARAMETERS_END();
    try {
        auto dimension =
            registry().open(std::string(ZSTR_VAL(blobPath), ZSTR_LEN(blobPath)),
                            std::string(ZSTR_VAL(noiseSettings), ZSTR_LEN(noiseSettings)),
                            std::string(ZSTR_VAL(biomeParameterList), ZSTR_LEN(biomeParameterList)),
                            static_cast<std::int64_t>(seed));
        object_init_ex(return_value, dimensionEntry);
        fetchDimension(Z_OBJ_P(return_value))->dimension = std::move(dimension);
    } catch (...) {
        throwAsPhp();
    }
}

static ZEND_METHOD(Stratum_Dimension, getMinY) {
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_LONG(fetchDimension(Z_OBJ_P(ZEND_THIS))->dimension->geometry().minY);
}

static ZEND_METHOD(Stratum_Dimension, getHeight) {
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_LONG(fetchDimension(Z_OBJ_P(ZEND_THIS))->dimension->geometry().height);
}

static ZEND_METHOD(Stratum_Dimension, encodeChunk) {
    zend_long chunkX = 0;
    zend_long chunkZ = 0;
    ZEND_PARSE_PARAMETERS_START(2, 2)
    Z_PARAM_LONG(chunkX)
    Z_PARAM_LONG(chunkZ)
    ZEND_PARSE_PARAMETERS_END();
    try {
        const auto& dimension = *fetchDimension(Z_OBJ_P(ZEND_THIS))->dimension;
        const std::vector<stratum::pmmp::EncodedSubChunk> encoded = stratum::pmmp::encodeChunk(
            dimension, static_cast<std::int32_t>(chunkX), static_cast<std::int32_t>(chunkZ));
        array_init_size(return_value, static_cast<std::uint32_t>(encoded.size()));
        for (const stratum::pmmp::EncodedSubChunk& sub : encoded) {
            zval entry;
            array_init_size(&entry, 2);
            if (sub.blocks.has_value()) {
                addLayer(&entry, "blocks", *sub.blocks);
            } else {
                add_assoc_null(&entry, "blocks");
            }
            addLayer(&entry, "biomes", sub.biomes);
            // Sub-chunk indices go negative (-4 to 19). PHP stores integer
            // keys as zend_ulong and reads them back as zend_long, so the
            // two's-complement conversion is what makes key -4 come out as -4.
            add_index_zval(return_value, static_cast<zend_ulong>(static_cast<zend_long>(sub.index)),
                           &entry);
        }
    } catch (...) {
        throwAsPhp();
    }
}

static const zend_function_entry stratum_functions[] = {
    ZEND_NS_NAMED_FE("Stratum", freezePipeline, ZEND_FN(stratum_freezePipeline),
                     arginfo_freezePipeline)
        ZEND_NS_NAMED_FE("Stratum", bedrockBlockState, ZEND_FN(stratum_bedrockBlockState),
                         arginfo_bedrockBlockState)
            ZEND_NS_NAMED_FE("Stratum", javaBlockStateCount, ZEND_FN(stratum_javaBlockStateCount),
                             arginfo_javaBlockStateCount) ZEND_FE_END};

static const zend_function_entry dimension_methods[] = {
    ZEND_ME(Stratum_Dimension, __construct, arginfo_Dimension___construct, ZEND_ACC_PRIVATE)
        ZEND_ME(Stratum_Dimension, open, arginfo_Dimension_open, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
            ZEND_ME(Stratum_Dimension, getMinY, arginfo_Dimension_getInt, ZEND_ACC_PUBLIC)
                ZEND_ME(Stratum_Dimension, getHeight, arginfo_Dimension_getInt, ZEND_ACC_PUBLIC)
                    ZEND_ME(Stratum_Dimension, encodeChunk, arginfo_Dimension_encodeChunk,
                            ZEND_ACC_PUBLIC) ZEND_FE_END};

static PHP_MINIT_FUNCTION(stratum) {
    zend_class_entry exceptionClass;
    INIT_NS_CLASS_ENTRY(exceptionClass, "Stratum", "GenerationException", nullptr);
    generationExceptionEntry =
        zend_register_internal_class_ex(&exceptionClass, spl_ce_RuntimeException);
    generationExceptionEntry->ce_flags |= ZEND_ACC_FINAL;

    std::memcpy(&dimensionHandlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
    dimensionHandlers.offset = XtOffsetOf(DimensionObject, std);
    dimensionHandlers.free_obj = dimensionFree;
    dimensionHandlers.clone_obj = nullptr;

    zend_class_entry dimensionClass;
    INIT_NS_CLASS_ENTRY(dimensionClass, "Stratum", "Dimension", dimension_methods);
    dimensionClass.create_object = dimensionCreate;
    dimensionEntry = zend_register_internal_class(&dimensionClass);
    dimensionEntry->ce_flags |=
        ZEND_ACC_FINAL | ZEND_ACC_NO_DYNAMIC_PROPERTIES | ZEND_ACC_NOT_SERIALIZABLE;
    return SUCCESS;
}

static PHP_MINFO_FUNCTION(stratum) {
    php_info_print_table_start();
    php_info_print_table_header(2, "stratum support", "enabled");
    php_info_print_table_row(2, "version", PHP_STRATUM_VERSION);
    php_info_print_table_end();
}

zend_module_entry stratum_module_entry = {
    STANDARD_MODULE_HEADER,
    "stratum",
    stratum_functions,
    PHP_MINIT(stratum),
    nullptr,
    nullptr,
    nullptr,
    PHP_MINFO(stratum),
    PHP_STRATUM_VERSION,
    STANDARD_MODULE_PROPERTIES,
};

#ifdef COMPILE_DL_STRATUM
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
#endif
ZEND_GET_MODULE(stratum)
#endif

// NOLINTEND(readability-identifier-naming,cppcoreguidelines-*,modernize-*,hicpp-*)

} // extern "C"
