# Derives the build's version identity from project(VERSION) plus the git
# working tree, and generates a private config header for src/version.cpp.
#
# project(VERSION) in CMakeLists.txt is the single source of truth for the
# release number; the git metadata only qualifies it:
#
#   OPENMOQ_VERSION       "0.3.13"                       always PROJECT_VERSION
#   OPENMOQ_VERSION_FULL  "0.3.13"                       HEAD is tagged v0.3.13
#                         "0.3.13-dev+gabc1234"          untagged commit
#                         "0.3.13-dev+gabc1234.dirty"    uncommitted changes
#   OPENMOQ_GIT_COMMIT    "abc1234" or "unknown"
#
# Builds without git (source tarballs, shallow checkouts without tags) fall
# back to the plain project version so the output is never empty.

find_package(Git QUIET)

set(OPENMOQ_VERSION "${PROJECT_VERSION}")
set(OPENMOQ_GIT_COMMIT "unknown")
set(_openmoq_git_dirty FALSE)
set(_openmoq_git_on_release_tag FALSE)

if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short=7 HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        RESULT_VARIABLE _openmoq_git_result
        OUTPUT_VARIABLE _openmoq_git_commit
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(_openmoq_git_result EQUAL 0 AND NOT _openmoq_git_commit STREQUAL "")
        set(OPENMOQ_GIT_COMMIT "${_openmoq_git_commit}")
    endif()

    # --dirty covers tracked modifications; untracked files do not taint.
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" describe --always --dirty
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        RESULT_VARIABLE _openmoq_git_result
        OUTPUT_VARIABLE _openmoq_git_describe
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(_openmoq_git_result EQUAL 0 AND _openmoq_git_describe MATCHES "-dirty$")
        set(_openmoq_git_dirty TRUE)
    endif()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" describe --tags --exact-match --match "v*" HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        RESULT_VARIABLE _openmoq_git_result
        OUTPUT_VARIABLE _openmoq_git_tag
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(_openmoq_git_result EQUAL 0)
        if(_openmoq_git_tag STREQUAL "v${PROJECT_VERSION}")
            set(_openmoq_git_on_release_tag TRUE)
        else()
            message(WARNING
                "OpenMOQ: HEAD is tagged ${_openmoq_git_tag} but project(VERSION) is "
                "${PROJECT_VERSION}; bump the VERSION in CMakeLists.txt to match the tag")
        endif()
    endif()

    # Re-run configure when HEAD moves so the embedded commit does not go stale.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/.git/HEAD")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" symbolic-ref -q HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        RESULT_VARIABLE _openmoq_git_result
        OUTPUT_VARIABLE _openmoq_git_ref
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(_openmoq_git_result EQUAL 0 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git/${_openmoq_git_ref}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/.git/${_openmoq_git_ref}")
    endif()
endif()

if(_openmoq_git_on_release_tag AND NOT _openmoq_git_dirty)
    set(OPENMOQ_VERSION_FULL "${OPENMOQ_VERSION}")
else()
    set(OPENMOQ_VERSION_FULL "${OPENMOQ_VERSION}-dev")
    if(NOT OPENMOQ_GIT_COMMIT STREQUAL "unknown")
        string(APPEND OPENMOQ_VERSION_FULL "+g${OPENMOQ_GIT_COMMIT}")
        if(_openmoq_git_dirty)
            string(APPEND OPENMOQ_VERSION_FULL ".dirty")
        endif()
    endif()
endif()

set(OPENMOQ_VERSION_CONFIG_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version_config.h.in"
    "${OPENMOQ_VERSION_CONFIG_DIR}/openmoq_version_config.h"
    @ONLY)

message(STATUS "OpenMOQ: version ${OPENMOQ_VERSION_FULL} (commit ${OPENMOQ_GIT_COMMIT})")

unset(_openmoq_git_result)
unset(_openmoq_git_commit)
unset(_openmoq_git_describe)
unset(_openmoq_git_tag)
unset(_openmoq_git_ref)
unset(_openmoq_git_dirty)
unset(_openmoq_git_on_release_tag)
