// Stratum — the JNI surface of `stratum.nukkit.NativePipeline`.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Marshaling and exception translation only, the same rule `ext/`'s zend
// shim follows: every function here converts its arguments, calls into
// `stratum::nukkit` (pipeline.hpp) and converts the result. The function
// names are JNI's own mangling of `stratum.nukkit.NativePipeline`'s native
// methods. `javac -h` writes their declarations from the Java source into
// `stratum_nukkit_NativePipeline.h` (ext-nukkit/CMakeLists.txt), included
// below, so a definition here that disagrees with its Java signature is a
// compile error rather than an UnsatisfiedLinkError at run time.
//
// Every C++ exception is caught here and rethrown as a Java
// `stratum.nukkit.StratumGenerationException`: one crossing the JNI boundary
// unhandled is undefined behaviour. A JNI call that fails leaves its own
// Java exception pending; the C++ exception raised after it only unwinds,
// and the pending, more specific Java exception is the one Java sees.
//
// Handles are heap objects passed to Java as a jlong, owned by the Java side
// until it calls the matching release. A pipeline handle holds a
// shared_ptr, so a bound handle made from it keeps the pipeline alive after
// the pipeline handle itself is released.
#include <stratum/data/resource_location.hpp>
#include <stratum/mapping/block_state.hpp>
#include <stratum/world/dimension.hpp>

#include <jni.h>
#include <stratum_nukkit/pipeline.hpp>
#include <stratum_nukkit_NativePipeline.h>

#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using stratum::nukkit::BlockIdTable;
using stratum::nukkit::NukkitError;
using stratum::nukkit::Pipeline;

constexpr const char* kExceptionClass = "stratum/nukkit/StratumGenerationException";

void throwJava(JNIEnv* env, const char* message) {
    if (env->ExceptionCheck() == JNI_TRUE) {
        return;
    }
    jclass exceptionClass = env->FindClass(kExceptionClass);
    if (exceptionClass != nullptr) {
        env->ThrowNew(exceptionClass, message);
    }
}

/// Runs @p body; any C++ exception becomes a pending Java one and the
/// function returns @p onError instead.
template<typename T, typename Body>
T guarded(JNIEnv* env, T onError, Body&& body) {
    try {
        return std::forward<Body>(body)();
    } catch (const std::exception& exception) {
        throwJava(env, exception.what());
    } catch (...) {
        throwJava(env, "stratum-nukkit: a native exception of unknown type");
    }
    return onError;
}

/// A Java string's modified UTF-8, released when this goes out of scope.
class JavaString {
public:
    JavaString(JNIEnv* env, jstring string) : env_(env), string_(string) {
        if (string == nullptr) {
            throw NukkitError("stratum-nukkit: a required string argument is null");
        }
        chars_ = env->GetStringUTFChars(string, nullptr);
        if (chars_ == nullptr) {
            throw NukkitError("stratum-nukkit: could not read a string argument");
        }
    }

    ~JavaString() {
        if (chars_ != nullptr) {
            env_->ReleaseStringUTFChars(string_, chars_);
        }
    }

    JavaString(const JavaString&) = delete;
    JavaString& operator=(const JavaString&) = delete;
    JavaString(JavaString&&) = delete;
    JavaString& operator=(JavaString&&) = delete;

    [[nodiscard]] std::string str() const { return chars_; }

private:
    JNIEnv* env_;
    jstring string_;
    const char* chars_ = nullptr;
};

struct PipelineHandle {
    std::shared_ptr<const Pipeline> pipeline;
};

struct BoundHandle {
    std::shared_ptr<const Pipeline> pipeline;
    BlockIdTable table;
};

// This IS what a JNI handle is: a jlong carrying a pointer across a boundary
// that has no pointer type of its own.
template<typename T>
T& fromHandle(const jlong handle, const char* what) {
    if (handle == 0) {
        throw NukkitError(std::string("stratum-nukkit: a released or never-opened ") + what +
                          " handle");
    }
    return *reinterpret_cast<T*>(handle); // NOLINT(performance-no-int-to-ptr)
}

template<typename T>
jlong toHandle(std::unique_ptr<T> object) {
    return reinterpret_cast<jlong>(object.release());
}

[[nodiscard]] std::vector<std::int32_t> readIntArray(JNIEnv* env, jintArray array) {
    if (array == nullptr) {
        throw NukkitError("stratum-nukkit: a required int[] argument is null");
    }
    static_assert(sizeof(jint) == sizeof(std::int32_t));
    std::vector<std::int32_t> values(static_cast<std::size_t>(env->GetArrayLength(array)));
    env->GetIntArrayRegion(array, 0, static_cast<jsize>(values.size()),
                           reinterpret_cast<jint*>(values.data()));
    if (env->ExceptionCheck() == JNI_TRUE) {
        throw NukkitError("stratum-nukkit: could not read an int[] argument");
    }
    return values;
}

[[nodiscard]] jintArray toIntArray(JNIEnv* env, const std::vector<std::int32_t>& values) {
    jintArray array = env->NewIntArray(static_cast<jsize>(values.size()));
    if (array == nullptr) {
        throw NukkitError("stratum-nukkit: could not allocate an int[]");
    }
    env->SetIntArrayRegion(array, 0, static_cast<jsize>(values.size()),
                           reinterpret_cast<const jint*>(values.data()));
    return array;
}

[[nodiscard]] jobjectArray toStringArray(JNIEnv* env, const std::vector<std::string>& values) {
    jclass stringClass = env->FindClass("java/lang/String");
    if (stringClass == nullptr) {
        throw NukkitError("stratum-nukkit: java.lang.String is not loadable");
    }
    jobjectArray array =
        env->NewObjectArray(static_cast<jsize>(values.size()), stringClass, nullptr);
    if (array == nullptr) {
        throw NukkitError("stratum-nukkit: could not allocate a String[]");
    }
    for (std::size_t i = 0; i < values.size(); ++i) {
        jstring element = env->NewStringUTF(values[i].c_str());
        if (element == nullptr) {
            throw NukkitError("stratum-nukkit: could not allocate a String");
        }
        env->SetObjectArrayElement(array, static_cast<jsize>(i), element);
        env->DeleteLocalRef(element);
    }
    return array;
}

/// Copies @p values into the caller's @p array, which must be exactly as
/// long: only after a fill succeeded, so a throwing fill leaves the Java
/// arrays as they were.
void writeIntArray(JNIEnv* env, jintArray array, const std::vector<std::int32_t>& values) {
    env->SetIntArrayRegion(array, 0, static_cast<jsize>(values.size()),
                           reinterpret_cast<const jint*>(values.data()));
}

} // namespace

extern "C" {

// NOLINTBEGIN(readability-identifier-naming) — JNI's own mangling names every
// function below; the JVM finds them by exactly these names.

JNIEXPORT void JNICALL Java_stratum_nukkit_NativePipeline_freeze(JNIEnv* env, jclass /*clazz*/,
                                                                 jstring versionRoot,
                                                                 jstring blobPath) {
    guarded(env, 0, [&] {
        const JavaString root(env, versionRoot);
        const JavaString blob(env, blobPath);
        stratum::nukkit::freezePack(root.str(), blob.str());
        return 0;
    });
}

JNIEXPORT jlong JNICALL Java_stratum_nukkit_NativePipeline_open(JNIEnv* env, jclass /*clazz*/,
                                                                jstring blobPath, jstring settings,
                                                                jstring biomes, const jlong seed) {
    return guarded(env, jlong{0}, [&] {
        const JavaString blob(env, blobPath);
        const JavaString settingsId(env, settings);
        const JavaString biomeSource(env, biomes);
        auto handle = std::make_unique<PipelineHandle>();
        handle->pipeline = Pipeline::open(
            blob.str(), stratum::data::ResourceLocation::parse(settingsId.str()),
            stratum::world::BiomeSource::parse(biomeSource.str()), static_cast<std::int64_t>(seed));
        return toHandle(std::move(handle));
    });
}

JNIEXPORT void JNICALL Java_stratum_nukkit_NativePipeline_release(JNIEnv* /*env*/, jclass /*clazz*/,
                                                                  const jlong handle) {
    delete reinterpret_cast<PipelineHandle*>(handle); // NOLINT(performance-no-int-to-ptr)
}

JNIEXPORT jint JNICALL Java_stratum_nukkit_NativePipeline_minY(JNIEnv* env, jclass /*clazz*/,
                                                               const jlong handle) {
    return guarded(env, jint{0}, [&] {
        return static_cast<jint>(fromHandle<PipelineHandle>(handle, "pipeline").pipeline->minY());
    });
}

JNIEXPORT jint JNICALL Java_stratum_nukkit_NativePipeline_height(JNIEnv* env, jclass /*clazz*/,
                                                                 const jlong handle) {
    return guarded(env, jint{0}, [&] {
        return static_cast<jint>(fromHandle<PipelineHandle>(handle, "pipeline").pipeline->height());
    });
}

JNIEXPORT jintArray JNICALL Java_stratum_nukkit_NativePipeline_emittableStateIds(
    JNIEnv* env, jclass /*clazz*/, const jlong handle) {
    return guarded(env, static_cast<jintArray>(nullptr), [&] {
        const Pipeline& pipeline = *fromHandle<PipelineHandle>(handle, "pipeline").pipeline;
        std::vector<std::int32_t> ids;
        ids.reserve(pipeline.emittableStateIds().size());
        for (const std::uint32_t id : pipeline.emittableStateIds()) {
            ids.push_back(static_cast<std::int32_t>(id));
        }
        return toIntArray(env, ids);
    });
}

JNIEXPORT jobjectArray JNICALL Java_stratum_nukkit_NativePipeline_emittableStateNames(
    JNIEnv* env, jclass /*clazz*/, const jlong handle) {
    return guarded(env, static_cast<jobjectArray>(nullptr), [&] {
        const Pipeline& pipeline = *fromHandle<PipelineHandle>(handle, "pipeline").pipeline;
        std::vector<std::string> names;
        names.reserve(pipeline.emittableStates().size());
        for (const stratum::settings::BlockState& state : pipeline.emittableStates()) {
            names.push_back(stratum::nukkit::describe(state));
        }
        return toStringArray(env, names);
    });
}

JNIEXPORT jintArray JNICALL Java_stratum_nukkit_NativePipeline_emittableBiomeIds(
    JNIEnv* env, jclass /*clazz*/, const jlong handle) {
    return guarded(env, static_cast<jintArray>(nullptr), [&] {
        const Pipeline& pipeline = *fromHandle<PipelineHandle>(handle, "pipeline").pipeline;
        std::vector<std::int32_t> ids;
        for (const Pipeline::Biome& biome : pipeline.emittableBiomes()) {
            ids.push_back(biome.bedrockId);
        }
        return toIntArray(env, ids);
    });
}

JNIEXPORT jobjectArray JNICALL Java_stratum_nukkit_NativePipeline_emittableBiomeNames(
    JNIEnv* env, jclass /*clazz*/, const jlong handle) {
    return guarded(env, static_cast<jobjectArray>(nullptr), [&] {
        const Pipeline& pipeline = *fromHandle<PipelineHandle>(handle, "pipeline").pipeline;
        std::vector<std::string> names;
        for (const Pipeline::Biome& biome : pipeline.emittableBiomes()) {
            names.push_back(biome.java.toString());
        }
        return toStringArray(env, names);
    });
}

JNIEXPORT jint JNICALL Java_stratum_nukkit_NativePipeline_javaBlockStateCount(JNIEnv* /*env*/,
                                                                              jclass /*clazz*/) {
    return static_cast<jint>(stratum::mapping::javaBlockStateCount());
}

JNIEXPORT jint JNICALL
Java_stratum_nukkit_NativePipeline_bedrockBlockStateVersion(JNIEnv* /*env*/, jclass /*clazz*/) {
    return static_cast<jint>(stratum::mapping::bedrockBlockStateVersion());
}

JNIEXPORT jobjectArray JNICALL Java_stratum_nukkit_NativePipeline_bedrockState(
    JNIEnv* env, jclass /*clazz*/, const jint javaStateId) {
    return guarded(env, static_cast<jobjectArray>(nullptr), [&] {
        if (javaStateId < 0) {
            throw NukkitError("stratum-nukkit: Java block state id " + std::to_string(javaStateId) +
                              " is negative");
        }
        const stratum::mapping::BedrockBlockState state =
            stratum::mapping::bedrockBlockState(static_cast<std::uint32_t>(javaStateId));
        std::vector<std::string> keys;
        std::vector<std::int32_t> types;
        std::vector<std::int32_t> integers;
        std::vector<std::string> strings;
        for (const stratum::mapping::BedrockStateValue& value : state.states) {
            keys.emplace_back(value.name);
            types.push_back(static_cast<std::int32_t>(value.type));
            integers.push_back(value.integer);
            strings.emplace_back(value.string);
        }
        jclass objectClass = env->FindClass("java/lang/Object");
        if (objectClass == nullptr) {
            throw NukkitError("stratum-nukkit: java.lang.Object is not loadable");
        }
        jobjectArray result = env->NewObjectArray(5, objectClass, nullptr);
        if (result == nullptr) {
            throw NukkitError("stratum-nukkit: could not allocate an Object[]");
        }
        jstring name = env->NewStringUTF(std::string(state.name).c_str());
        if (name == nullptr) {
            throw NukkitError("stratum-nukkit: could not allocate a String");
        }
        env->SetObjectArrayElement(result, 0, name);
        env->SetObjectArrayElement(result, 1, toStringArray(env, keys));
        env->SetObjectArrayElement(result, 2, toIntArray(env, types));
        env->SetObjectArrayElement(result, 3, toIntArray(env, integers));
        env->SetObjectArrayElement(result, 4, toStringArray(env, strings));
        return result;
    });
}

JNIEXPORT jlong JNICALL Java_stratum_nukkit_NativePipeline_bind(JNIEnv* env, jclass /*clazz*/,
                                                                const jlong handle,
                                                                jintArray javaStateIds,
                                                                jintArray fullIds) {
    return guarded(env, jlong{0}, [&] {
        const PipelineHandle& pipeline = fromHandle<PipelineHandle>(handle, "pipeline");
        const std::vector<std::int32_t> ids = readIntArray(env, javaStateIds);
        const std::vector<std::int32_t> resolved = readIntArray(env, fullIds);
        auto bound = std::make_unique<BoundHandle>(BoundHandle{
            .pipeline = pipeline.pipeline, .table = BlockIdTable::fromResolved(ids, resolved)});
        bound->pipeline->requireComplete(bound->table);
        return toHandle(std::move(bound));
    });
}

JNIEXPORT void JNICALL Java_stratum_nukkit_NativePipeline_releaseBound(JNIEnv* /*env*/,
                                                                       jclass /*clazz*/,
                                                                       const jlong handle) {
    delete reinterpret_cast<BoundHandle*>(handle); // NOLINT(performance-no-int-to-ptr)
}

JNIEXPORT jintArray JNICALL Java_stratum_nukkit_NativePipeline_fill(
    JNIEnv* env, jclass /*clazz*/, const jlong handle, const jint chunkX, const jint chunkZ,
    jintArray fullBlockIds, jintArray biomeIds) {
    return guarded(env, static_cast<jintArray>(nullptr), [&] {
        const BoundHandle& bound = fromHandle<BoundHandle>(handle, "bound pipeline");
        if (fullBlockIds == nullptr || biomeIds == nullptr) {
            throw NukkitError("stratum-nukkit: fill() needs both output arrays");
        }
        std::vector<std::int32_t> blocks(
            static_cast<std::size_t>(env->GetArrayLength(fullBlockIds)));
        std::vector<std::int32_t> biomes(static_cast<std::size_t>(env->GetArrayLength(biomeIds)));
        std::vector<std::int32_t> fluids;
        bound.pipeline->fill(static_cast<std::int32_t>(chunkX), static_cast<std::int32_t>(chunkZ),
                             bound.table, blocks, biomes, &fluids);
        writeIntArray(env, fullBlockIds, blocks);
        writeIntArray(env, biomeIds, biomes);
        return toIntArray(env, fluids);
    });
}

// NOLINTEND(readability-identifier-naming)

} // extern "C"
