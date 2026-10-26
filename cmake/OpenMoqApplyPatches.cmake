# Applies a provider's tracked patch series to a git checkout. Run as a
# FetchContent PATCH_COMMAND:
#   cmake -DGIT_EXECUTABLE=<git> -DPATCH_DIR=<dir with series> -DSOURCE_DIR=<checkout>
#         [-DSTAMP_FILE=<path> -DFINGERPRINT=<value>] -P OpenMoqApplyPatches.cmake
# The series file lists patch names, one per line (blank lines and # comments
# ignored), in the order they apply. Same rules as libmoq's
# scripts/provider_patches.sh: a patch that applies is applied, one that is
# already in (it reverses cleanly) is skipped, anything else stops the build.
foreach(_required IN ITEMS GIT_EXECUTABLE PATCH_DIR SOURCE_DIR)
    if(NOT ${_required})
        message(FATAL_ERROR "OpenMoqApplyPatches: ${_required} is required")
    endif()
endforeach()

# The step reruns whenever FetchContent updates the checkout, even when the
# pin did not move and the update kept the patches. Checking each patch in
# reverse cannot detect that: a patch whose lines a later patch also changes
# no longer reverses cleanly on its own. The stamp records a complete series.
if(STAMP_FILE AND FINGERPRINT AND EXISTS "${STAMP_FILE}")
    file(READ "${STAMP_FILE}" _applied)
    string(STRIP "${_applied}" _applied)
    # A clean tree carries no patches whatever the stamp says.
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" status --porcelain
        OUTPUT_VARIABLE _changes ERROR_QUIET)
    if(_applied STREQUAL FINGERPRINT AND NOT _changes STREQUAL "")
        message(STATUS "OpenMOQ: patch series already applied to ${SOURCE_DIR}")
        return()
    endif()
endif()

file(STRINGS "${PATCH_DIR}/series" _lines)
foreach(_line IN LISTS _lines)
    string(REGEX REPLACE "#.*$" "" _line "${_line}")
    string(STRIP "${_line}" _name)
    if(_name STREQUAL "")
        continue()
    endif()
    set(_patch "${PATCH_DIR}/${_name}")
    if(NOT EXISTS "${_patch}")
        message(FATAL_ERROR "OpenMoqApplyPatches: missing patch ${_patch}")
    endif()
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" apply --check "${_patch}"
        RESULT_VARIABLE _applies OUTPUT_QUIET ERROR_QUIET)
    if(_applies EQUAL 0)
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" apply "${_patch}"
            RESULT_VARIABLE _applied)
        if(NOT _applied EQUAL 0)
            message(FATAL_ERROR "OpenMoqApplyPatches: ${_name} failed to apply to ${SOURCE_DIR}")
        endif()
        message(STATUS "OpenMOQ: applied ${_name}")
        continue()
    endif()
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" apply --reverse --check "${_patch}"
        RESULT_VARIABLE _already OUTPUT_QUIET ERROR_QUIET)
    if(_already EQUAL 0)
        message(STATUS "OpenMOQ: ${_name} already applied")
    else()
        message(FATAL_ERROR "OpenMoqApplyPatches: ${_name} conflicts with ${SOURCE_DIR}")
    endif()
endforeach()

# Records the pin and series the checkout now carries (see
# openmoq_discard_stale_patches in CMakeLists.txt). Written only once the
# whole series is in.
if(STAMP_FILE AND FINGERPRINT)
    file(WRITE "${STAMP_FILE}" "${FINGERPRINT}\n")
endif()
