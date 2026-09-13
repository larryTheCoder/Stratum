// Stratum — the JNI surface `StratumGenerator.java`'s native methods bind to.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
//
// Deliberately thin: every function here does argument marshaling and
// exception translation only, and calls straight into
// `stratum_nukkit::Pipeline` (pipeline.hpp) for the actual work — the same
// "marshaling only, no generation logic" rule `ext/`'s own README states
// for the PocketMine-MP binding. Method names follow JNI's own mangling
// convention for `cn.nukkit.generator.stratum.StratumGenerator`'s native
// methods; if that class or package ever moves, these names have to move
// with it; `javac -h` regenerates them from the Java source rather than by
// hand once a real Nukkit build exists to run it against (not yet — see
// this directory's own README).
//
// EVERY THROW `Pipeline`/`resolveNukkitFullId` can raise, and every
// exception `lib/`'s own loaders can raise underneath them, is caught here
// and turned into a real Java exception via `ThrowNew` — an exception
// crossing the JNI boundary unhandled is undefined behaviour, not a
// language-level guarantee the way it is on either side alone.
#include <jni.h>
#include <stratum_nukkit/pipeline.hpp>

#include <exception>
#include <memory>

namespace {

using stratum::nukkit::Pipeline;

constexpr const char* kExceptionClass = "cn/nukkit/generator/stratum/StratumGenerationException";

/// Turns @p pointer's C++ exception into a thrown Java one and returns
/// @p onError. Called from a catch block, never on its own.
template<typename T>
T translateException(JNIEnv* env, const std::exception& exception, T onError) {
    // A pending exception already means a previous JNI call itself failed
    // (e.g. NewStringUTF running out of memory) — that exception is more
    // specific than anything this function could raise, so it is left in
    // place rather than overwritten.
    if (env->ExceptionCheck() == JNI_FALSE) {
        env->ThrowNew(env->FindClass(kExceptionClass), exception.what());
    }
    return onError;
}

// This IS what a JNI handle is: a jlong round-tripping a pointer across a
// boundary that has no pointer type of its own, not an optimisation
// opportunity forgone.
Pipeline* asPipeline(const jlong handle) {
    return reinterpret_cast<Pipeline*>(handle); // NOLINT(performance-no-int-to-ptr)
}

} // namespace

extern "C" {

// NOLINTBEGIN(readability-identifier-naming) — JNI's own mangling
// convention names every function below; matching it exactly is what makes
// the JVM able to find them at all, not a style choice this project gets to
// make.
JNIEXPORT jlong JNICALL Java_cn_nukkit_generator_stratum_StratumGenerator_nativeCompile(
    JNIEnv* env, jclass /*clazz*/, jstring packDir, jstring dimension, jlong worldSeed) {
    const char* packDirChars = env->GetStringUTFChars(packDir, nullptr);
    const char* dimensionChars = env->GetStringUTFChars(dimension, nullptr);
    jlong result = 0;
    try {
        std::unique_ptr<Pipeline> pipeline =
            Pipeline::compile(std::filesystem::path(packDirChars),
                              stratum::data::ResourceLocation::parse(dimensionChars), worldSeed);
        // Ownership crosses into the handle Java now holds — released back
        // to a std::unique_ptr in nativeDestroy, never left to a raw
        // `delete` mismatched against how it was allocated.
        result = reinterpret_cast<jlong>(pipeline.release());
    } catch (const std::exception& exception) {
        result = translateException<jlong>(env, exception, 0);
    }
    env->ReleaseStringUTFChars(packDir, packDirChars);
    env->ReleaseStringUTFChars(dimension, dimensionChars);
    return result;
}

JNIEXPORT jint JNICALL Java_cn_nukkit_generator_stratum_StratumGenerator_nativeMinY(
    JNIEnv* /*env*/, jclass /*clazz*/, const jlong handle) {
    return asPipeline(handle)->minY();
}

JNIEXPORT jint JNICALL Java_cn_nukkit_generator_stratum_StratumGenerator_nativeHeight(
    JNIEnv* /*env*/, jclass /*clazz*/, const jlong handle) {
    return asPipeline(handle)->height();
}

JNIEXPORT void JNICALL Java_cn_nukkit_generator_stratum_StratumGenerator_nativeFill(
    JNIEnv* env, jclass /*clazz*/, const jlong handle, const jint chunkX, const jint chunkZ,
    jintArray fullBlockIds, jintArray biomeIds) {
    const jsize blockLength = env->GetArrayLength(fullBlockIds);
    const jsize biomeLength = env->GetArrayLength(biomeIds);
    jint* blockElements = env->GetIntArrayElements(fullBlockIds, nullptr);
    jint* biomeElements = env->GetIntArrayElements(biomeIds, nullptr);

    // static_assert rather than a runtime check: this binding is never
    // built for a platform where the two differ, and CLAUDE.md's
    // determinism rules already forbid pretending otherwise elsewhere.
    static_assert(sizeof(jint) == sizeof(std::int32_t));

    try {
        asPipeline(handle)->fill(
            chunkX, chunkZ,
            std::span<std::int32_t>(reinterpret_cast<std::int32_t*>(blockElements),
                                    static_cast<std::size_t>(blockLength)),
            std::span<std::int32_t>(reinterpret_cast<std::int32_t*>(biomeElements),
                                    static_cast<std::size_t>(biomeLength)));
    } catch (const std::exception& exception) {
        translateException<std::nullptr_t>(env, exception, nullptr);
    }

    // JNI_COMMIT (not 0) even on the exception path: a chunk that threw
    // partway through must not hand the caller whatever this run's buffer
    // held over from a PREVIOUS successful fill() of a different position
    // — the caller is about to see an exception and must not also read
    // stale data through the array it passed in.
    env->ReleaseIntArrayElements(fullBlockIds, blockElements, JNI_COMMIT);
    env->ReleaseIntArrayElements(biomeIds, biomeElements, JNI_COMMIT);
}

JNIEXPORT void JNICALL Java_cn_nukkit_generator_stratum_StratumGenerator_nativeDestroy(
    JNIEnv* /*env*/, jclass /*clazz*/, const jlong handle) {
    delete asPipeline(handle);
}

// NOLINTEND(readability-identifier-naming)

} // extern "C"
