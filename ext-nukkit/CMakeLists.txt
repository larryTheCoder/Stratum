# ext-nukkit/ — CloudburstMC/Nukkit JNI binding (Milestone M5, SPEC section 9).
#
# Three layers, each needing more than the one before:
#
#   stratum_nukkit_pipeline  plain C++, always built; tests/unit/ links it
#                            into stratum_unit_tests, so `ctest --preset dev`
#                            covers it on every platform with no JVM at all.
#   stratum_nukkit           the JNI shim, a shared library, and the Java
#                            classes with no Nukkit type in them, tested
#                            through real JNI in a bare JVM. Needs a JDK
#                            (javac and jni.h): -DSTRATUM_BUILD_EXT_NUKKIT=ON,
#                            JAVA_HOME pointing at one.
#   the Nukkit plugin        compiled against a real Nukkit server jar
#                            (STRATUM_NUKKIT_JAR, from tools/nukkit-dev), and
#                            the headless smoke run against it. Without the
#                            jar these tests are still registered, and skip
#                            saying so.

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

if(NOT STRATUM_BUILD_EXT_NUKKIT)
    return()
endif()

# --- The JNI shim -------------------------------------------------------------

find_package(JNI REQUIRED)
find_package(Java 1.8 REQUIRED COMPONENTS Development)

# Nukkit's floor is Java 8 (its build.gradle.kts), so every class is compiled
# for it. javac 9+ warns that 8 is obsolete; that warning is about javac, not
# this code, and is the one class switched off.
set(STRATUM_JAVAC_FLAGS --release 8 -encoding UTF-8 -Xlint:all,-options)

set(java_dir "${CMAKE_CURRENT_SOURCE_DIR}/java")
set(java_out "${CMAKE_CURRENT_BINARY_DIR}/java")
file(GLOB core_sources CONFIGURE_DEPENDS "${java_dir}/core/stratum/nukkit/*.java")
file(GLOB test_sources CONFIGURE_DEPENDS "${java_dir}/test/stratum/nukkit/*.java")

# The core classes, and the JNI header javac writes from NativePipeline's
# native methods. jni_bridge.cpp includes that header, so a native method
# whose Java signature and C++ definition disagree is a compile error rather
# than an UnsatisfiedLinkError at run time.
set(jni_header "${java_out}/jni/stratum_nukkit_NativePipeline.h")
add_custom_command(
    OUTPUT "${java_out}/core.stamp" "${jni_header}"
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${java_out}/core" "${java_out}/jni"
    COMMAND "${Java_JAVAC_EXECUTABLE}" ${STRATUM_JAVAC_FLAGS} -Werror
            -d "${java_out}/core" -h "${java_out}/jni" ${core_sources}
    COMMAND "${CMAKE_COMMAND}" -E touch "${java_out}/core.stamp"
    DEPENDS ${core_sources}
    COMMENT "javac: ext-nukkit core classes and JNI header"
    VERBATIM
)
add_custom_target(stratum_nukkit_java_core DEPENDS "${java_out}/core.stamp" "${jni_header}")

add_library(stratum_nukkit SHARED
    src/jni_bridge.cpp
)
add_dependencies(stratum_nukkit stratum_nukkit_java_core)
# SYSTEM: jni.h and javac's generated header are not this project's code and
# are not held to its warning set.
target_include_directories(stratum_nukkit SYSTEM PRIVATE "${java_out}/jni" ${JNI_INCLUDE_DIRS})
target_link_libraries(stratum_nukkit PRIVATE stratum::nukkit_pipeline)
stratum_configure_target(stratum_nukkit)

if(STRATUM_BUILD_TESTS)
    # The JNI surface in a bare JVM, no Nukkit: freezing, opening, the
    # refusals, a synthetic id table, and fills on several threads against
    # one (java/test/stratum/nukkit/NativePipelineCheck.java).
    add_custom_command(
        OUTPUT "${java_out}/test.stamp"
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${java_out}/test"
        COMMAND "${Java_JAVAC_EXECUTABLE}" ${STRATUM_JAVAC_FLAGS} -Werror
                -d "${java_out}/test" ${core_sources} ${test_sources}
        COMMAND "${CMAKE_COMMAND}" -E touch "${java_out}/test.stamp"
        DEPENDS ${core_sources} ${test_sources}
        COMMENT "javac: ext-nukkit JNI check"
        VERBATIM
    )
    add_custom_target(stratum_nukkit_java_test ALL DEPENDS "${java_out}/test.stamp")

    set(STRATUM_FIXTURES_DIR "${CMAKE_SOURCE_DIR}/.fixtures" CACHE PATH
        "Location of locally generated, never-committed vanilla fixtures")
    # Since JDK 24 loading a native library without this prints a warning
    # that a later JDK will make an error; 21 accepts the flag, 11 does not.
    set(native_access "")
    if(Java_VERSION VERSION_GREATER_EQUAL 21)
        set(native_access "--enable-native-access=ALL-UNNAMED")
    endif()
    add_test(NAME ext-nukkit.jni
        COMMAND "${Java_JAVA_EXECUTABLE}" ${native_access}
            "-Dstratum.nukkit.library=$<TARGET_FILE:stratum_nukkit>"
            -cp "${java_out}/test"
            stratum.nukkit.NativePipelineCheck
            "${STRATUM_FIXTURES_DIR}/${STRATUM_MINECRAFT_VERSION}"
            "${CMAKE_CURRENT_BINARY_DIR}/jni-scratch"
    )
    set_tests_properties(ext-nukkit.jni PROPERTIES
        LABELS "nukkit"
        SKIP_RETURN_CODE 77
        TIMEOUT 600
    )
endif()

# --- The Nukkit plugin, against a real server jar ---------------------------

set(STRATUM_NUKKIT_JAR "" CACHE FILEPATH
    "Nukkit server jar to compile the plugin against and smoke-test in (tools/nukkit-dev --print-jar)")

# javac's class path separator: CMake would split a literal ';' into two
# arguments, so Windows gets it from a generator expression.
if(WIN32)
    set(classpath_separator "$<SEMICOLON>")
else()
    set(classpath_separator ":")
endif()

file(GLOB plugin_sources CONFIGURE_DEPENDS "${java_dir}/plugin/stratum/nukkit/*.java")
file(GLOB smoke_sources CONFIGURE_DEPENDS "${java_dir}/smoke/stratum/nukkit/smoke/*.java")
set(plugin_jar "${CMAKE_CURRENT_BINARY_DIR}/stratum-nukkit.jar")
set(smoke_jar "${CMAKE_CURRENT_BINARY_DIR}/stratum-nukkit-smoke.jar")

if(STRATUM_NUKKIT_JAR AND EXISTS "${STRATUM_NUKKIT_JAR}")
    configure_file("${java_dir}/plugin/plugin.yml.in" "${java_out}/plugin-resources/plugin.yml" @ONLY)
    configure_file("${java_dir}/smoke/plugin.yml.in" "${java_out}/smoke-resources/plugin.yml" @ONLY)

    # Not -Werror: the warnings javac raises here can come from Nukkit's own
    # class files (annotations it does not ship), which no change here can
    # fix. They are printed, never hidden.
    add_custom_command(
        OUTPUT "${plugin_jar}"
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${java_out}/plugin"
        COMMAND "${Java_JAVAC_EXECUTABLE}" ${STRATUM_JAVAC_FLAGS}
                -cp "${STRATUM_NUKKIT_JAR}" -d "${java_out}/plugin"
                ${core_sources} ${plugin_sources}
        COMMAND "${Java_JAR_EXECUTABLE}" --create --file "${plugin_jar}"
                -C "${java_out}/plugin" .
                -C "${java_out}/plugin-resources" plugin.yml
        DEPENDS ${core_sources} ${plugin_sources} "${java_dir}/plugin/plugin.yml.in"
                "${STRATUM_NUKKIT_JAR}"
        COMMENT "javac + jar: the Stratum Nukkit plugin, against ${STRATUM_NUKKIT_JAR}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${smoke_jar}"
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${java_out}/smoke"
        COMMAND "${Java_JAVAC_EXECUTABLE}" ${STRATUM_JAVAC_FLAGS}
                -cp "${STRATUM_NUKKIT_JAR}${classpath_separator}${java_out}/plugin"
                -d "${java_out}/smoke" ${smoke_sources}
        COMMAND "${Java_JAR_EXECUTABLE}" --create --file "${smoke_jar}"
                -C "${java_out}/smoke" .
                -C "${java_out}/smoke-resources" plugin.yml
        DEPENDS ${smoke_sources} "${java_dir}/smoke/plugin.yml.in" "${plugin_jar}"
        COMMENT "javac + jar: the Stratum Nukkit smoke plugin"
        VERBATIM
    )
    add_custom_target(stratum_nukkit_plugin ALL DEPENDS "${plugin_jar}" "${smoke_jar}" stratum_nukkit)

    if(STRATUM_BUILD_TESTS)
        # The plugin jar holds the generator, the plugin and its plugin.yml.
        add_test(NAME ext-nukkit.plugin
            COMMAND "${Java_JAR_EXECUTABLE}" --list --file "${plugin_jar}")
        set_tests_properties(ext-nukkit.plugin PROPERTIES
            LABELS "nukkit"
            PASS_REGULAR_EXPRESSION "stratum/nukkit/StratumGenerator\\.class.*plugin\\.yml|plugin\\.yml.*stratum/nukkit/StratumGenerator\\.class"
        )
        add_test(NAME ext-nukkit.smoke
            COMMAND "${CMAKE_SOURCE_DIR}/tools/nukkit-dev" smoke
                --jar "${STRATUM_NUKKIT_JAR}"
                --plugin "${plugin_jar}"
                --smoke-plugin "${smoke_jar}"
                --library "$<TARGET_FILE:stratum_nukkit>"
                --pack "${STRATUM_FIXTURES_DIR}/${STRATUM_MINECRAFT_VERSION}"
                --java "${Java_JAVA_EXECUTABLE}"
        )
        set_tests_properties(ext-nukkit.smoke PROPERTIES
            LABELS "nukkit"
            SKIP_RETURN_CODE 77
            TIMEOUT 1800
            RUN_SERIAL TRUE
        )
    endif()
elseif(STRATUM_BUILD_TESTS)
    # Registered anyway, so a run without the jar reports what it did not
    # check instead of passing quietly.
    foreach(skipped plugin smoke)
        add_test(NAME ext-nukkit.${skipped}
            COMMAND "${CMAKE_COMMAND}" -E echo
                "SKIP ext-nukkit.${skipped}: no Nukkit server jar. Set STRATUM_NUKKIT_JAR (tools/nukkit-dev --print-jar) and reconfigure."
        )
        set_tests_properties(ext-nukkit.${skipped} PROPERTIES
            LABELS "nukkit"
            SKIP_REGULAR_EXPRESSION "^SKIP "
        )
    endforeach()
endif()
