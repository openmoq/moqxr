include_guard(GLOBAL)

# Resolve an installed picoquic for OPENMOQ_USE_SYSTEM_PICOQUIC=ON.
#
# picoquic installs a CMake CONFIG package (picoquic-config.cmake) that exports
# picoquic::picoquic-core, picoquic::picoquic-log and picoquic::picohttp-core
# as imported static libraries. Their link interfaces already carry picotls,
# OpenSSL and the thread library, so nothing about picotls needs to be located
# here; the installed picoquic was built against one specific picotls and the
# export records it.
#
# What the installed package must provide beyond the default picoquic install:
#
#   picoquic_internal.h (and the picohash.h it includes). picoquic treats
#   these as private headers and does not install them, but
#   src/transport/picoquic_close_drain.h reads picoquic_stream_head_t and
#   picoquic_cnx_t fields through them. They MUST come from the exact
#   picoquic revision that produced the installed libraries: reading those
#   structs through a header from any other revision is a silent ABI
#   mismatch. Packagers should install both files next to picoquic.h from the
#   same source tree they built the libraries from.
#
# On success this sets, in the caller's scope:
#   OPENMOQ_PICOQUIC_CORE_TARGET / OPENMOQ_PICOQUIC_LOG_TARGET /
#   OPENMOQ_PICOHTTP_TARGET     -- the namespaced imported targets to link
#   OPENMOQ_PICOQUIC_EXTRA_INCLUDE_DIRS -- directories the Publisher sources
#                                  need in addition to the targets' usage
#                                  requirements (the private-header directory
#                                  when it is not already on the export's
#                                  include path)
#   OPENMOQ_SYSTEM_PICOQUIC_VERSION -- picoquic_VERSION from the package
function(openmoq_find_system_picoquic)
    find_package(picoquic CONFIG REQUIRED)

    foreach(_required_target IN ITEMS
            picoquic::picoquic-core
            picoquic::picoquic-log
            picoquic::picohttp-core)
        if(NOT TARGET "${_required_target}")
            message(FATAL_ERROR
                "OPENMOQ_USE_SYSTEM_PICOQUIC=ON: the installed picoquic package at "
                "${picoquic_DIR} does not export ${_required_target}. The Publisher "
                "needs picoquic built and installed with -DBUILD_HTTP=ON (HTTP/3 + "
                "WebTransport) and -DBUILD_LOGLIB=ON, with all three libraries in "
                "the CMake export set. Reinstall picoquic accordingly, point "
                "CMAKE_PREFIX_PATH or picoquic_DIR at such an install, or build "
                "picoquic from source by configuring with "
                "-DOPENMOQ_USE_SYSTEM_PICOQUIC=OFF.")
        endif()
    endforeach()

    # Collect the export's public include directories; the private headers
    # are expected alongside picoquic.h in one of them.
    set(_export_include_dirs "")
    foreach(_target IN ITEMS picoquic::picoquic-core picoquic::picohttp-core)
        get_target_property(_dirs "${_target}" INTERFACE_INCLUDE_DIRECTORIES)
        if(_dirs)
            list(APPEND _export_include_dirs ${_dirs})
        endif()
    endforeach()
    if(picoquic_INCLUDE_DIR)
        list(APPEND _export_include_dirs "${picoquic_INCLUDE_DIR}")
    endif()
    list(REMOVE_DUPLICATES _export_include_dirs)

    find_path(OPENMOQ_PICOQUIC_INTERNAL_INCLUDE_DIR
        NAMES picoquic_internal.h
        HINTS ${_export_include_dirs}
        DOC "Directory holding picoquic's private picoquic_internal.h from the installed picoquic revision")
    mark_as_advanced(OPENMOQ_PICOQUIC_INTERNAL_INCLUDE_DIR)

    if(NOT OPENMOQ_PICOQUIC_INTERNAL_INCLUDE_DIR)
        string(REPLACE ";" ", " _searched "${_export_include_dirs}")
        message(FATAL_ERROR
            "OPENMOQ_USE_SYSTEM_PICOQUIC=ON: picoquic_internal.h was not found "
            "(searched the installed package's include directories: ${_searched}). "
            "The Publisher's close-drain logic reads picoquic connection and "
            "stream state through this private header, so the installed picoquic "
            "package must ship picoquic_internal.h and picohash.h from the same "
            "source revision as its libraries, next to picoquic.h. Install them "
            "there, or set -DOPENMOQ_PICOQUIC_INTERNAL_INCLUDE_DIR=<dir> to a "
            "directory containing those two files from that exact revision. "
            "Alternatively build picoquic from source with "
            "-DOPENMOQ_USE_SYSTEM_PICOQUIC=OFF.")
    endif()

    # picoquic_internal.h includes "picohash.h" and "picosplay.h" relative to
    # itself. picosplay.h is part of picoquic's normal install; picohash.h is
    # private and must have been installed together with picoquic_internal.h.
    foreach(_companion IN ITEMS picohash.h picosplay.h)
        if(NOT EXISTS "${OPENMOQ_PICOQUIC_INTERNAL_INCLUDE_DIR}/${_companion}")
            message(FATAL_ERROR
                "OPENMOQ_USE_SYSTEM_PICOQUIC=ON: found picoquic_internal.h in "
                "${OPENMOQ_PICOQUIC_INTERNAL_INCLUDE_DIR} but not ${_companion}, "
                "which it includes. Install ${_companion} from the same picoquic "
                "revision into that directory.")
        endif()
    endforeach()

    set(_extra_include_dirs "")
    if(NOT OPENMOQ_PICOQUIC_INTERNAL_INCLUDE_DIR IN_LIST _export_include_dirs)
        set(_extra_include_dirs "${OPENMOQ_PICOQUIC_INTERNAL_INCLUDE_DIR}")
    endif()

    if(picoquic_VERSION)
        # picoquic's config file passes PROJECT_VERSION through PATH_VARS, so
        # the value arrives prefixed with the install prefix
        # ("<prefix>/0.0.0-<hash>"); keep only the version component.
        get_filename_component(_version "${picoquic_VERSION}" NAME)
    else()
        set(_version "unknown")
    endif()

    set(OPENMOQ_PICOQUIC_CORE_TARGET picoquic::picoquic-core PARENT_SCOPE)
    set(OPENMOQ_PICOQUIC_LOG_TARGET picoquic::picoquic-log PARENT_SCOPE)
    set(OPENMOQ_PICOHTTP_TARGET picoquic::picohttp-core PARENT_SCOPE)
    set(OPENMOQ_PICOQUIC_EXTRA_INCLUDE_DIRS "${_extra_include_dirs}" PARENT_SCOPE)
    set(OPENMOQ_SYSTEM_PICOQUIC_VERSION "${_version}" PARENT_SCOPE)

    message(STATUS
        "OpenMOQ: picoquic system package ${_version} from ${picoquic_DIR} "
        "(private headers: ${OPENMOQ_PICOQUIC_INTERNAL_INCLUDE_DIR})")
endfunction()
