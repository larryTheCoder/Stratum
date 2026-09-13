# ext-nukkit/ — CloudburstMC/Nukkit JNI binding (Milestone M5, SPEC section 9).
#
# Two targets on purpose: `stratum_nukkit_pipeline` is plain C++, buildable
# and unit-testable with no JVM at all (pipeline.hpp's own header explains
# why); `stratum_nukkit` is the thin JNI shim `StratumGenerator.java`'s
# native methods actually load, and needs a JDK's jni.h to build. Splitting
# them is what lets tests/unit/ exercise the real pipeline logic without a
# Java toolchain in the loop at all.

find_package(JNI REQUIRED)

add_library(stratum_nukkit_pipeline STATIC
    src/pipeline.cpp
)

add_library(stratum::nukkit_pipeline ALIAS stratum_nukkit_pipeline)

target_include_directories(stratum_nukkit_pipeline PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(stratum_nukkit_pipeline PUBLIC
    stratum::core
    stratum::mapping
)

stratum_configure_target(stratum_nukkit_pipeline)

add_library(stratum_nukkit SHARED
    src/jni_bridge.cpp
)

target_link_libraries(stratum_nukkit PRIVATE
    stratum::nukkit_pipeline
    JNI::JNI
)

stratum_configure_target(stratum_nukkit)
