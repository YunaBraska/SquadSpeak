# Generate a CTest-time fragment from QtTest's public function inventory.
cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CHANNEL_TEST_EXECUTABLE OR NOT DEFINED OUTPUT_FILE)
    message(FATAL_ERROR "CHANNEL_TEST_EXECUTABLE and OUTPUT_FILE are required")
endif()
if(NOT EXISTS "${CHANNEL_TEST_EXECUTABLE}")
    message(FATAL_ERROR "channel_tests executable is missing: ${CHANNEL_TEST_EXECUTABLE}")
endif()

execute_process(
    COMMAND "${CHANNEL_TEST_EXECUTABLE}" -functions
    TIMEOUT 30
    RESULT_VARIABLE result
    OUTPUT_VARIABLE inventory
    ERROR_VARIABLE error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT "${result}" STREQUAL "0" OR inventory STREQUAL "")
    message(FATAL_ERROR "channel_tests -functions failed: ${error}")
endif()

string(REPLACE "\n" ";" lines "${inventory}")
set(contents "# Generated from channel_tests -functions. Do not edit.\n")
set(count 0)
foreach(line IN LISTS lines)
    if(line STREQUAL "")
        continue()
    endif()
    string(REGEX REPLACE "\r$" "" line "${line}")
    string(REGEX MATCH "^([A-Za-z0-9_]+)[(][)]$" match "${line}")
    if(NOT match)
        message(FATAL_ERROR "Unexpected channel_tests -functions line: ${line}")
    endif()
    set(function "${CMAKE_MATCH_1}")
    set(name "channel_contract_${function}")
    string(APPEND contents "add_test([=[${name}]=]")
    if(TEST_LAUNCHER)
        # CTest requires a readable executable. macOS sudo is execute-only.
        string(APPEND contents " [=[${CMAKE_COMMAND}]=] -E env")
    endif()
    foreach(argument IN LISTS TEST_LAUNCHER)
        string(APPEND contents " [=[${argument}]=]")
    endforeach()
    string(APPEND contents " [=[${CHANNEL_TEST_EXECUTABLE}]=] [=[${function}]=])\n")
    string(APPEND contents "set_tests_properties([=[${name}]=] PROPERTIES TIMEOUT 300)\n")
    if(function MATCHES "^(sixtyFourClientsHaveOneConsistentRosterAndAudioFanout|sixtyFourVideoViewersKeepIndependentQualityAndVoice|mediaAdmissionExceeds64AcrossOwnedChannels)$"
       OR function MATCHES "(screenQualityAdaptsForOnlyTheSlowReceiver|screenUsesUdpWhileReliableTrafficIsDelayed|voiceUsesUdpWhileReliableTrafficIsDelayedThenFallsBackAndRecovers)")
        string(APPEND contents "set_tests_properties([=[${name}]=] PROPERTIES RUN_SERIAL TRUE COST 1000)\n")
    endif()
    if(function MATCHES "^(boundedDiscovery|destroyingDiscovery|directJoinUsesDefaultPort)")
        string(APPEND contents "set_tests_properties([=[${name}]=] PROPERTIES RESOURCE_LOCK channel_fixed_ports)\n")
    endif()
    math(EXPR count "${count} + 1")
endforeach()
if(count LESS 1)
    message(FATAL_ERROR "channel_tests -functions returned no functions")
endif()
file(WRITE "${OUTPUT_FILE}" "${contents}")
message(STATUS "Discovered ${count} channel QtTest functions")
