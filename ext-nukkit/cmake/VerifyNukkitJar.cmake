# Refuses a Nukkit server jar that is not, byte for byte, the one
# tools/nukkit-dev pins. Nothing is compiled against a jar this has not
# passed: javac reads the class path, and the pin is what the owner approved.
#
# Used twice, with STRATUM_NUKKIT_JAR and STRATUM_NUKKIT_DEV (the script's
# path) set: included by ext-nukkit/CMakeLists.txt at configure time, and run
# with `cmake -P` before every javac against the jar, so a jar replaced after
# configuring is caught too.

# The pin's own line: a literal digest, or empty. (`pin` also prints a
# NUKKIT_SHA256 line, holding a shell variable, for the owner to paste.)
file(STRINGS "${STRATUM_NUKKIT_DEV}" _stratum_pin_line REGEX "^readonly NUKKIT_SHA256=\"[0-9a-f]*\"$")
list(LENGTH _stratum_pin_line _stratum_pin_count)
if(NOT _stratum_pin_count EQUAL 1
   OR NOT _stratum_pin_line MATCHES "^readonly NUKKIT_SHA256=\"([0-9a-f]*)\"$")
    message(FATAL_ERROR "ext-nukkit: cannot read the one NUKKIT_SHA256 pin from ${STRATUM_NUKKIT_DEV}")
endif()
set(_stratum_pinned "${CMAKE_MATCH_1}")
if(_stratum_pinned STREQUAL "")
    message(FATAL_ERROR
        "ext-nukkit: tools/nukkit-dev pins no Nukkit build yet, so STRATUM_NUKKIT_JAR "
        "(${STRATUM_NUKKIT_JAR}) cannot be verified; refusing to compile anything against it. "
        "Leave STRATUM_NUKKIT_JAR empty and the plugin and smoke tests skip, saying so.")
endif()
if(NOT EXISTS "${STRATUM_NUKKIT_JAR}" OR IS_DIRECTORY "${STRATUM_NUKKIT_JAR}")
    message(FATAL_ERROR "ext-nukkit: STRATUM_NUKKIT_JAR names ${STRATUM_NUKKIT_JAR}, which is not a file")
endif()
file(SHA256 "${STRATUM_NUKKIT_JAR}" _stratum_actual)
if(NOT _stratum_actual STREQUAL _stratum_pinned)
    message(FATAL_ERROR
        "ext-nukkit: ${STRATUM_NUKKIT_JAR} has SHA-256 ${_stratum_actual}, not the "
        "${_stratum_pinned} tools/nukkit-dev pins: refused")
endif()
unset(_stratum_pin_line)
unset(_stratum_pin_count)
unset(_stratum_pinned)
unset(_stratum_actual)
