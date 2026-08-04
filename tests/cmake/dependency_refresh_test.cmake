cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED OPENMOQ_SOURCE_DIR)
    message(FATAL_ERROR "OPENMOQ_SOURCE_DIR is required")
endif()

if(NOT DEFINED OPENMOQ_TEST_TEMP_DIR)
    message(FATAL_ERROR "OPENMOQ_TEST_TEMP_DIR is required")
endif()

include("${OPENMOQ_SOURCE_DIR}/cmake/OpenMoqDependencies.cmake")

function(expect_refresh expected description)
    openmoq_dependency_refresh_due(${ARGN} RESULT actual)
    if(NOT "${actual}" STREQUAL "${expected}")
        message(FATAL_ERROR
            "${description}: expected refresh=${expected}, got ${actual}")
    endif()
endfunction()

file(REMOVE_RECURSE "${OPENMOQ_TEST_TEMP_DIR}")
file(MAKE_DIRECTORY "${OPENMOQ_TEST_TEMP_DIR}")
set(stamp_file "${OPENMOQ_TEST_TEMP_DIR}/last-refresh")

expect_refresh(TRUE "a missing stamp must refresh"
    STAMP_FILE "${stamp_file}"
    INTERVAL_HOURS 24
    NOW_EPOCH 100000)

openmoq_record_dependency_refresh(
    STAMP_FILE "${stamp_file}"
    NOW_EPOCH 100000)

expect_refresh(FALSE "a recent successful refresh must be reused"
    STAMP_FILE "${stamp_file}"
    INTERVAL_HOURS 24
    NOW_EPOCH 186399)

openmoq_record_dependency_refresh_if_due(
    REFRESH_DUE FALSE
    STAMP_FILE "${stamp_file}"
    NOW_EPOCH 150000)
file(READ "${stamp_file}" unchanged_stamp)
string(STRIP "${unchanged_stamp}" unchanged_stamp)
if(NOT unchanged_stamp STREQUAL "100000")
    message(FATAL_ERROR
        "a cache-only configure must not postpone the next refresh")
endif()

expect_refresh(TRUE "the 24-hour boundary must refresh"
    STAMP_FILE "${stamp_file}"
    INTERVAL_HOURS 24
    NOW_EPOCH 186400)

file(WRITE "${stamp_file}" "not-an-epoch\n")
expect_refresh(TRUE "a corrupt stamp must refresh"
    STAMP_FILE "${stamp_file}"
    INTERVAL_HOURS 24
    NOW_EPOCH 200000)

file(REMOVE_RECURSE "${OPENMOQ_TEST_TEMP_DIR}")
