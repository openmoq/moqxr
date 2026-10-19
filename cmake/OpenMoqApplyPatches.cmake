# Applies a provider's tracked patch series to a git checkout. Run as a
# FetchContent PATCH_COMMAND:
#   cmake -DGIT_EXECUTABLE=<git> -DPATCH_DIR=<dir with series> -DSOURCE_DIR=<checkout>
#         -P OpenMoqApplyPatches.cmake
# The series file lists patch names, one per line (blank lines and # comments
# ignored), in the order they apply. Same rules as libmoq's
# scripts/provider_patches.sh: a patch that applies is applied, one that is
# already in (it reverses cleanly) is skipped, anything else stops the build.
foreach(_required IN ITEMS GIT_EXECUTABLE PATCH_DIR SOURCE_DIR)
    if(NOT ${_required})
        message(FATAL_ERROR "OpenMoqApplyPatches: ${_required} is required")
    endif()
endforeach()

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
