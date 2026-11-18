# Release versions follow UTC YYYY.M.D, as in the shared Swift release flow.
# An explicit version lets every matrix job package the same build at midnight.
if(NOT DEFINED SQUADSPEAK_VERSION)
    string(TIMESTAMP SQUADSPEAK_VERSION "%Y.%m.%d" UTC)
    string(REGEX REPLACE "\\.0([0-9])" ".\\1" SQUADSPEAK_VERSION "${SQUADSPEAK_VERSION}")
endif()
if(NOT SQUADSPEAK_VERSION MATCHES "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)(-SNAPSHOT)?$")
    message(FATAL_ERROR "SQUADSPEAK_VERSION must have three numeric parts without leading zeroes, optionally followed by -SNAPSHOT.")
endif()
# CMake and native bundle metadata require numeric components. The application
# retains the full label, so a test package cannot masquerade as a stable build.
string(REGEX REPLACE "-SNAPSHOT$" "" SQUADSPEAK_NUMERIC_VERSION "${SQUADSPEAK_VERSION}")

# App Store Connect distinguishes repeated uploads by CFBundleVersion. Keep
# that build number separate from the user-facing release version so a retried
# stable release remains uploadable without changing its public version.
if(NOT DEFINED SQUADSPEAK_APPLE_BUILD_NUMBER)
    set(SQUADSPEAK_APPLE_BUILD_NUMBER "${SQUADSPEAK_NUMERIC_VERSION}")
endif()
if(NOT SQUADSPEAK_APPLE_BUILD_NUMBER MATCHES "^(0|[1-9][0-9]*)(\\.(0|[1-9][0-9]*))?(\\.(0|[1-9][0-9]*))?$")
    message(FATAL_ERROR "SQUADSPEAK_APPLE_BUILD_NUMBER must contain one to three numeric parts without leading zeroes.")
endif()
if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    message("${SQUADSPEAK_VERSION}")
endif()
