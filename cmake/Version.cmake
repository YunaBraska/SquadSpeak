# Release versions follow UTC YYYY.M.D, as in the shared Swift release flow.
# An explicit version lets every matrix job package the same build at midnight.
if(NOT DEFINED SQUADSPEAK_VERSION)
    string(TIMESTAMP SQUADSPEAK_VERSION "%Y.%m.%d" UTC)
    string(REGEX REPLACE "\\.0([0-9])" ".\\1" SQUADSPEAK_VERSION "${SQUADSPEAK_VERSION}")
endif()
if(NOT SQUADSPEAK_VERSION MATCHES "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)$")
    message(FATAL_ERROR "SQUADSPEAK_VERSION must have three numeric parts without leading zeroes (YYYY.M.D).")
endif()
if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    message("${SQUADSPEAK_VERSION}")
endif()
