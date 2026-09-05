cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR
        "In-source builds are not supported. Configure with: cmake -S . -B build")
endif()

project(openmoq_publisher
    VERSION 0.3.22
    DESCRIPTION "OpenMOQ contribution project: C++20 publisher for fragmented MP4 to CMSF/MOQT"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(OPENMOQ_BUILD_TESTS "Build OpenMOQ publisher tests" ON)
option(OPENMOQ_BUILD_EXAMPLES "Build OpenMOQ publisher example executables" OFF)
option(OPENMOQ_ENABLE_PICOQUIC "Enable picoquic transport integration when picoquic is available" ON)
option(OPENMOQ_RUN_PICOQUIC_SMOKE_TESTS "Build and run picoquic loopback smoke tests" OFF)
# Compiles picoquic (and this project) with PICOQUIC_WITH_THREAD_CHECK so any
# picoquic API call from a thread other than the packet-loop thread aborts.
# picoquic's internals (notably the wake-time splay tree) are single-threaded;
# a cross-thread call is a data race that can silently corrupt the scheduler.
# Defaults to the smoke-test setting so loopback CI runs catch regressions.
option(OPENMOQ_PICOQUIC_THREAD_CHECK
    "Abort on cross-thread picoquic API calls (test builds)"
    ${OPENMOQ_RUN_PICOQUIC_SMOKE_TESTS})
option(OPENMOQ_ENABLE_SRT "Enable libsrt ingest support when libsrt is available" ON)
# Temporary migration gate: route the production Publisher through libmoq instead
# of the legacy MoqtSession path. Requires libmoq to be available. OFF keeps the
# old/moxygen publish path as the default until the migration is accepted; the
# libmoq integration code still builds and is tested whenever libmoq is present.
option(OPENMOQ_USE_LIBMOQ_PUBLISHER "Use libmoq as the production publish backend (requires libmoq)" OFF)

# Install rules default to ON only for top-level builds; when moqxr is vendored
# via add_subdirectory or FetchContent, the consuming project owns the install
# stage. PROJECT_IS_TOP_LEVEL requires CMake 3.21, so derive it to keep the
# 3.20 minimum.
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(_openmoq_is_top_level ON)
else()
    set(_openmoq_is_top_level OFF)
endif()
option(OPENMOQ_INSTALL
    "Generate install rules for the openmoq-publisher CLI" ${_openmoq_is_top_level})
# The development install ships the Publisher API headers plus a
# self-contained static archive: the vendored picotls/picoquic (and moq5 when
# enabled) static libraries are merged into the installed libopenmoq_publisher
# archive, so consumers only link the genuinely external dependencies
# (OpenSSL, plus libsrt when OPENMOQ_ENABLE_SRT is ON).
option(OPENMOQ_INSTALL_DEVELOPMENT
    "Additionally install a self-contained static Publisher library and public headers" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(OpenMoqDependencies)
include(OpenMoqVersion)
include(FetchContent)

# The managed picotls and picoquic trees are build-time-only dependencies:
# their targets must stay out of the default build, and -- critically -- none
# of their install() rules may be generated. `cmake --install` must ship only
# first-party artifacts, and picoquic additionally bakes the configure-time
# ${CMAKE_INSTALL_PREFIX} into absolute install DESTINATIONs that ignore
# `cmake --install --prefix`. Directory-level EXCLUDE_FROM_ALL provides
# exactly this on every supported CMake version; FetchContent only learned to
# forward it from FetchContent_Declare() in CMake 3.28, so older versions
# take the FetchContent_Populate() + add_subdirectory(EXCLUDE_FROM_ALL) path
# instead. (Single-argument FetchContent_Populate() is only deprecated
# starting with CMake 3.30, which never reaches that branch.)
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.28")
    set(_openmoq_fetchcontent_exclude_from_all EXCLUDE_FROM_ALL)
else()
    set(_openmoq_fetchcontent_exclude_from_all "")
endif()
macro(openmoq_make_dependency_available name)
    if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.28")
        FetchContent_MakeAvailable(${name})
    else()
        FetchContent_GetProperties(${name})
        if(NOT ${name}_POPULATED)
            FetchContent_Populate(${name})
            add_subdirectory("${${name}_SOURCE_DIR}" "${${name}_BINARY_DIR}"
                EXCLUDE_FROM_ALL)
        endif()
    endif()
endmacro()

set(OPENMOQ_DEPENDENCY_REFRESH_INTERVAL_HOURS "24" CACHE STRING
    "Minimum hours between checks for newer managed dependency branch heads")
set(OPENMOQ_PICOQUIC_SOURCE_DIR "" CACHE PATH
    "Optional picoquic source override; empty fetches private-octopus/picoquic master")
set(OPENMOQ_PICOTLS_SOURCE_DIR "" CACHE PATH
    "Optional picotls source override; empty fetches h2o/picotls master")
set(OPENMOQ_LIBMOQ_SOURCE_DIR "" CACHE PATH
    "Optional moq5/libmoq source override; empty fetches openmoq/moq5 main when selected")

# Migrate build trees configured before managed dependencies became the
# default. Their auto-detected paths were cached with a "defaults to" help
# string and otherwise look indistinguishable from a deliberate override.
foreach(_openmoq_source_override IN ITEMS
        OPENMOQ_PICOQUIC_SOURCE_DIR
        OPENMOQ_PICOTLS_SOURCE_DIR
        OPENMOQ_LIBMOQ_SOURCE_DIR)
    get_property(_openmoq_override_help CACHE "${_openmoq_source_override}"
        PROPERTY HELPSTRING)
    if(_openmoq_override_help MATCHES "defaults to")
        set("${_openmoq_source_override}" "" CACHE PATH
            "Optional source override; empty uses the managed upstream dependency" FORCE)
        message(STATUS
            "OpenMOQ: cleared legacy auto-detected ${_openmoq_source_override} cache entry")
    endif()
endforeach()
unset(_openmoq_source_override)
unset(_openmoq_override_help)

# Older moqxr revisions forced this FetchContent escape hatch to the
# auto-detected picotls checkout. Managed mode must remove that stale redirect;
# callers should use OPENMOQ_PICOTLS_SOURCE_DIR for a deliberate override.
if(NOT OPENMOQ_PICOTLS_SOURCE_DIR AND FETCHCONTENT_SOURCE_DIR_PICOTLS)
    unset(FETCHCONTENT_SOURCE_DIR_PICOTLS CACHE)
    unset(FETCHCONTENT_SOURCE_DIR_PICOTLS)
    message(STATUS
        "OpenMOQ: cleared legacy FETCHCONTENT_SOURCE_DIR_PICOTLS cache entry")
endif()

set(_openmoq_dependency_stamp "${CMAKE_CURRENT_BINARY_DIR}/_deps/openmoq-last-refresh")
openmoq_dependency_refresh_due(
    STAMP_FILE "${_openmoq_dependency_stamp}"
    INTERVAL_HOURS "${OPENMOQ_DEPENDENCY_REFRESH_INTERVAL_HOURS}"
    RESULT _openmoq_dependency_refresh_due)
if(_openmoq_dependency_refresh_due)
    set(FETCHCONTENT_UPDATES_DISCONNECTED OFF)
    message(STATUS
        "OpenMOQ: checking managed dependency branches (refresh interval: "
        "${OPENMOQ_DEPENDENCY_REFRESH_INTERVAL_HOURS} hours)")
else()
    set(FETCHCONTENT_UPDATES_DISCONNECTED ON)
    message(STATUS
        "OpenMOQ: managed dependency branch check is current; using the build cache")
endif()
set(_openmoq_managed_dependency_used OFF)

set(OPENMOQ_HAS_SRT OFF)
if(OPENMOQ_ENABLE_SRT)
    find_path(OPENMOQ_SRT_INCLUDE_DIR NAMES srt/srt.h)
    find_library(OPENMOQ_SRT_LIBRARY NAMES srt)
    if(OPENMOQ_SRT_INCLUDE_DIR AND OPENMOQ_SRT_LIBRARY)
        set(OPENMOQ_HAS_SRT ON)
    else()
        message(STATUS "libsrt not found; building without SRT ingest runtime")
    endif()
endif()

# OpenSSL coherency: an explicit prefix must apply to both headers and libraries
# used by picotls, picoquic, and libmoq. The default intentionally follows
# CMake's current system OpenSSL instead of auto-pinning the retired 1.1 branch.
set(OPENMOQ_OPENSSL_ROOT_DIR "" CACHE PATH
    "Optional OpenSSL prefix shared by picoquic, picotls, and libmoq")
if(OPENMOQ_OPENSSL_ROOT_DIR)
    # Cache + FORCE so all dependency subtrees resolve the same prefix.
    set(OPENSSL_ROOT_DIR "${OPENMOQ_OPENSSL_ROOT_DIR}" CACHE PATH "" FORCE)
    set(CMAKE_FIND_FRAMEWORK LAST)
    message(STATUS "OpenMOQ: pinning OpenSSL to ${OPENMOQ_OPENSSL_ROOT_DIR}")
else()
    message(STATUS "OpenMOQ: no pinned OpenSSL found; relying on CMake default FindOpenSSL")
endif()

set(OPENMOQ_HAS_PICOQUIC OFF)
set(OPENMOQ_RESOLVED_PICOQUIC_SOURCE_DIR "")
set(OPENMOQ_RESOLVED_PICOTLS_SOURCE_DIR "")

function(openmoq_sanitize_target_link_directories target_name)
    if(NOT TARGET "${target_name}")
        return()
    endif()

    foreach(property_name IN ITEMS INTERFACE_LINK_DIRECTORIES LINK_DIRECTORIES)
        get_target_property(property_value "${target_name}" "${property_name}")
        if(NOT property_value OR property_value STREQUAL "property_value-NOTFOUND")
            continue()
        endif()

        set(filtered_value "${property_value}")
        # picotls can inject literal "(" and ")" via pkg-config parsing; after
        # CMake normalization these may appear as source-prefixed "/.../(" and
        # "/.../)" paths. Drop both raw and normalized forms.
        list(FILTER filtered_value EXCLUDE REGEX "^\\($|^\\)$|.*/\\($|.*/\\)$")
        set_target_properties("${target_name}" PROPERTIES "${property_name}" "${filtered_value}")
    endforeach()
endfunction()

if(OPENMOQ_ENABLE_PICOQUIC)
    # picotls still declares compatibility with old CMake versions; newer CMake
    # releases require an explicit policy floor when configuring that subproject.
    set(CMAKE_POLICY_VERSION_MINIMUM "3.5" CACHE STRING "" FORCE)
    set(PICOQUIC_FETCH_PTLS ON CACHE BOOL "" FORCE)
    set(picoquic_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    if(OPENMOQ_PICOQUIC_THREAD_CHECK)
        set(WITH_THREAD_CHECK ON CACHE BOOL "" FORCE)
    else()
        set(WITH_THREAD_CHECK OFF CACHE BOOL "" FORCE)
    endif()
    set(BUILD_DEMO OFF CACHE BOOL "" FORCE)
    set(BUILD_HTTP ON CACHE BOOL "" FORCE)
    set(BUILD_PQBENCH OFF CACHE BOOL "" FORCE)
    set(BUILD_LOGLIB ON CACHE BOOL "" FORCE)
    set(BUILD_LOGREADER OFF CACHE BOOL "" FORCE)

    if(OPENMOQ_PICOTLS_SOURCE_DIR)
        get_filename_component(OPENMOQ_RESOLVED_PICOTLS_SOURCE_DIR
            "${OPENMOQ_PICOTLS_SOURCE_DIR}" ABSOLUTE)
        if(NOT EXISTS "${OPENMOQ_RESOLVED_PICOTLS_SOURCE_DIR}/CMakeLists.txt")
            message(FATAL_ERROR
                "OPENMOQ_PICOTLS_SOURCE_DIR does not contain CMakeLists.txt: "
                "${OPENMOQ_RESOLVED_PICOTLS_SOURCE_DIR}")
        endif()
        FetchContent_Declare(picotls
            SOURCE_DIR "${OPENMOQ_RESOLVED_PICOTLS_SOURCE_DIR}"
            ${_openmoq_fetchcontent_exclude_from_all})
    else()
        set(OPENMOQ_RESOLVED_PICOTLS_SOURCE_DIR
            "${CMAKE_CURRENT_BINARY_DIR}/_deps/picotls-src")
        FetchContent_Declare(picotls
            GIT_REPOSITORY https://github.com/h2o/picotls.git
            GIT_TAG master
            GIT_SHALLOW TRUE
            GIT_PROGRESS TRUE
            GIT_SUBMODULES_RECURSE TRUE
            GIT_REMOTE_UPDATE_STRATEGY CHECKOUT
            SOURCE_DIR "${OPENMOQ_RESOLVED_PICOTLS_SOURCE_DIR}"
            ${_openmoq_fetchcontent_exclude_from_all})
        set(_openmoq_managed_dependency_used ON)
    endif()

    if(WIN32)
        # picotls.h includes "wincompat.h" on _WINDOWS but picotls's own CMakeLists.txt
        # does not add picotlsvs/picotls to every target that transitively includes it.
        include_directories(BEFORE
            "${OPENMOQ_RESOLVED_PICOTLS_SOURCE_DIR}/picotlsvs/picotls")
    endif()
    if(NOT WIN32)
        # Keep picotls isolated from malformed Brotli pkg-config link-directory
        # entries observed on macOS while its targets are configured.
        set(_openmoq_saved_pkg_config_libdir "$ENV{PKG_CONFIG_LIBDIR}")
        set(_openmoq_saved_pkg_config_path "$ENV{PKG_CONFIG_PATH}")
        set(ENV{PKG_CONFIG_LIBDIR} "${CMAKE_CURRENT_BINARY_DIR}/openmoq-empty-pkgconfig")
        set(ENV{PKG_CONFIG_PATH} "${CMAKE_CURRENT_BINARY_DIR}/openmoq-empty-pkgconfig")
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/openmoq-empty-pkgconfig")
    endif()
    openmoq_make_dependency_available(picotls)
    if(NOT WIN32)
        set(ENV{PKG_CONFIG_LIBDIR} "${_openmoq_saved_pkg_config_libdir}")
        set(ENV{PKG_CONFIG_PATH} "${_openmoq_saved_pkg_config_path}")
        unset(_openmoq_saved_pkg_config_libdir)
        unset(_openmoq_saved_pkg_config_path)
    endif()

    if(WIN32)
        # picotls's cli, bench, and test executables use POSIX headers not available
        # on Windows. Exclude them from the default build; the libraries remain.
        foreach(_picotls_exe IN ITEMS
                cli
                test-openssl.t
                test-minicrypto.t
                test-fusion.t
                test-mbedtls.t
                ptlsbench
                fuzz-asn1
                fuzz-server-hello
                fuzz-client-hello)
            if(TARGET "${_picotls_exe}")
                set_target_properties("${_picotls_exe}" PROPERTIES EXCLUDE_FROM_ALL ON)
            endif()
        endforeach()
        if(TARGET picotls-core AND
                EXISTS "${OPENMOQ_RESOLVED_PICOTLS_SOURCE_DIR}/picotlsvs/picotls/wintimeofday.c")
            target_sources(picotls-core PRIVATE
                "${OPENMOQ_RESOLVED_PICOTLS_SOURCE_DIR}/picotlsvs/picotls/wintimeofday.c")
        endif()
        if(TARGET picotls-minicrypto)
            target_link_libraries(picotls-minicrypto bcrypt)
        endif()
    endif()

    if(OPENMOQ_PICOQUIC_SOURCE_DIR)
        get_filename_component(OPENMOQ_RESOLVED_PICOQUIC_SOURCE_DIR
            "${OPENMOQ_PICOQUIC_SOURCE_DIR}" ABSOLUTE)
        if(NOT EXISTS "${OPENMOQ_RESOLVED_PICOQUIC_SOURCE_DIR}/CMakeLists.txt")
            message(FATAL_ERROR
                "OPENMOQ_PICOQUIC_SOURCE_DIR does not contain CMakeLists.txt: "
                "${OPENMOQ_RESOLVED_PICOQUIC_SOURCE_DIR}")
        endif()
        # Build-time-only dependency: EXCLUDE_FROM_ALL keeps its install()
        # rules out of the generated install scripts.
        add_subdirectory("${OPENMOQ_RESOLVED_PICOQUIC_SOURCE_DIR}"
            "${CMAKE_CURRENT_BINARY_DIR}/picoquic" EXCLUDE_FROM_ALL)
    else()
        set(OPENMOQ_RESOLVED_PICOQUIC_SOURCE_DIR
            "${CMAKE_CURRENT_BINARY_DIR}/_deps/picoquic-src")
        FetchContent_Declare(picoquic
            GIT_REPOSITORY https://github.com/private-octopus/picoquic.git
            GIT_TAG master
            GIT_SHALLOW TRUE
            GIT_PROGRESS TRUE
            GIT_SUBMODULES_RECURSE TRUE
            GIT_REMOTE_UPDATE_STRATEGY CHECKOUT
            SOURCE_DIR "${OPENMOQ_RESOLVED_PICOQUIC_SOURCE_DIR}"
            ${_openmoq_fetchcontent_exclude_from_all})
        openmoq_make_dependency_available(picoquic)
        set(_openmoq_managed_dependency_used ON)
    endif()

    # Sanitize after both dependency projects have configured their targets.
    openmoq_sanitize_target_link_directories(picotls-core)
    set(OPENMOQ_HAS_PICOQUIC ON)
endif()

# -- libmoq integration -------------------------------------------------
# Build moqxr against the libmoq service tier (moq::service). This is
# added AFTER picoquic above on purpose: libmoq's find_package(Picoquic) uses
# "already-defined target" as its first resolution step, so it reuses the
# picoquic-core target moqxr already created instead of add_subdirectory'ing
# picoquic a second time (which would collide on target names). By default moqxr
# still publishes via its own (legacy) transport; the Publisher only routes
# through libmoq when -DOPENMOQ_USE_LIBMOQ_PUBLISHER=ON. Linking libmoq always
# validates that the service tier builds and links into this project.

set(OPENMOQ_HAS_LIBMOQ OFF)
set(OPENMOQ_RESOLVED_LIBMOQ_SOURCE_DIR "")
set(_openmoq_configure_libmoq OFF)
set(_openmoq_managed_libmoq OFF)
if(OPENMOQ_LIBMOQ_SOURCE_DIR)
    get_filename_component(OPENMOQ_RESOLVED_LIBMOQ_SOURCE_DIR
        "${OPENMOQ_LIBMOQ_SOURCE_DIR}" ABSOLUTE)
    if(NOT EXISTS "${OPENMOQ_RESOLVED_LIBMOQ_SOURCE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "OPENMOQ_LIBMOQ_SOURCE_DIR does not contain CMakeLists.txt: "
            "${OPENMOQ_RESOLVED_LIBMOQ_SOURCE_DIR}")
    endif()
    set(_openmoq_configure_libmoq ON)
elseif(OPENMOQ_USE_LIBMOQ_PUBLISHER)
    set(OPENMOQ_RESOLVED_LIBMOQ_SOURCE_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/_deps/moq5-src")
    FetchContent_Declare(moq5
        GIT_REPOSITORY https://github.com/openmoq/moq5.git
        GIT_TAG main
        GIT_SHALLOW TRUE
        GIT_PROGRESS TRUE
        GIT_SUBMODULES_RECURSE TRUE
        GIT_REMOTE_UPDATE_STRATEGY CHECKOUT
        # Populate the checkout without adding it automatically. We add the
        # actual root below with EXCLUDE_FROM_ALL so standalone moq5 examples
        # do not become part of moqxr's default build.
        SOURCE_SUBDIR openmoq-fetch-only
        SOURCE_DIR "${OPENMOQ_RESOLVED_LIBMOQ_SOURCE_DIR}")
    set(_openmoq_configure_libmoq ON)
    set(_openmoq_managed_libmoq ON)
    set(_openmoq_managed_dependency_used ON)
endif()

if(_openmoq_configure_libmoq)
    # Service tier (endpoint + media sender/receiver) and its prerequisites.
    set(MOQ_BUILD_SERVICE      ON CACHE BOOL "" FORCE)
    set(MOQ_BUILD_MSF          ON CACHE BOOL "" FORCE)
    set(MOQ_BUILD_MEDIA_OBJECT ON CACHE BOOL "" FORCE)  # required by MOQ_BUILD_SERVICE
    # Keep libmoq's own test/sim suite out of moqxr's CTest run.
    set(MOQ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(MOQ_BUILD_SIM   OFF CACHE BOOL "" FORCE)

    if(OPENMOQ_HAS_PICOQUIC)
        # Usable raw QUIC + WebTransport: enable the picoquic adapter + threaded
        # helper AND the picoquic WebTransport adapter + managed helper (the test
        # relay is WebTransport-only). Map moqxr's picoquic/picotls inputs onto
        # libmoq's FindPicoquic hints; picohttp-core (built here with BUILD_HTTP)
        # satisfies the pico-WT HTTP/3 requirement.
        set(MOQ_BUILD_ADAPTER_PICOQUIC ON CACHE BOOL "" FORCE)
        set(MOQ_BUILD_PQ_THREADED      ON CACHE BOOL "" FORCE)
        set(MOQ_BUILD_ADAPTER_PICO_WT  ON CACHE BOOL "" FORCE)
        set(MOQ_BUILD_PICO_WT_MANAGED  ON CACHE BOOL "" FORCE)
        set(MOQ_PICOQUIC_SOURCE_DIR "${OPENMOQ_RESOLVED_PICOQUIC_SOURCE_DIR}" CACHE PATH "" FORCE)
        if(NOT MOQ_PICOTLS_PREFIX)
            set(MOQ_PICOTLS_PREFIX "${OPENMOQ_RESOLVED_PICOTLS_SOURCE_DIR}" CACHE PATH "" FORCE)
        endif()
    else()
        message(STATUS "libmoq: picoquic not available; building core/service tier without the raw QUIC adapter")
    endif()

    if(_openmoq_managed_libmoq)
        FetchContent_MakeAvailable(moq5)
        add_subdirectory("${OPENMOQ_RESOLVED_LIBMOQ_SOURCE_DIR}"
            "${moq5_BINARY_DIR}" EXCLUDE_FROM_ALL)
    else()
        add_subdirectory("${OPENMOQ_RESOLVED_LIBMOQ_SOURCE_DIR}"
            "${CMAKE_CURRENT_BINARY_DIR}/libmoq" EXCLUDE_FROM_ALL)
    endif()
    set(OPENMOQ_HAS_LIBMOQ ON)
else()
    message(STATUS
        "libmoq was not selected and no OPENMOQ_LIBMOQ_SOURCE_DIR override was provided")
endif()

if(OPENMOQ_HAS_PICOQUIC)
    if(OPENMOQ_PICOQUIC_SOURCE_DIR)
        set(_openmoq_picoquic_ref "local override")
    else()
        set(_openmoq_picoquic_ref "master")
    endif()
    if(OPENMOQ_PICOTLS_SOURCE_DIR)
        set(_openmoq_picotls_ref "local override")
    else()
        set(_openmoq_picotls_ref "master")
    endif()
    openmoq_report_git_dependency(picoquic
        "${OPENMOQ_RESOLVED_PICOQUIC_SOURCE_DIR}" "${_openmoq_picoquic_ref}")
    openmoq_report_git_dependency(picotls
        "${OPENMOQ_RESOLVED_PICOTLS_SOURCE_DIR}" "${_openmoq_picotls_ref}")
endif()
if(OPENMOQ_HAS_LIBMOQ)
    if(OPENMOQ_LIBMOQ_SOURCE_DIR)
        set(_openmoq_libmoq_ref "local override")
    else()
        set(_openmoq_libmoq_ref "main")
    endif()
    openmoq_report_git_dependency(moq5
        "${OPENMOQ_RESOLVED_LIBMOQ_SOURCE_DIR}" "${_openmoq_libmoq_ref}")
endif()

add_library(openmoq_publisher_lib STATIC
    src/cat4moq.cpp
    src/cenc.cpp
    src/cli_options.cpp
    src/cmaf_segmenter.cpp
    src/cmsf_packager.cpp
    src/drm_config.cpp
    src/endpoint_failover.cpp
    src/live_srt_ingest.cpp
    src/live_srt_config.cpp
    src/live_dash_ingest.cpp
    src/moq_draft.cpp
    src/msf_catalog.cpp
    src/msf_url.cpp
    src/mp4_box.cpp
    src/publisher_api.cpp
    src/transport/libmoq_publisher.cpp
    src/transport/moqt_control_messages.cpp
    src/transport/moqt_session.cpp
    src/transport/picoquic_client.cpp
    src/transport/publisher_transport.cpp
    src/transport/tls_verification.cpp
    src/transport/webtransport_client.cpp
    src/version.cpp
)

set_target_properties(openmoq_publisher_lib PROPERTIES
    OUTPUT_NAME openmoq_publisher
)

target_include_directories(openmoq_publisher_lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${OPENMOQ_VERSION_CONFIG_DIR}
)

target_compile_features(openmoq_publisher_lib PUBLIC cxx_std_20)

if(OPENMOQ_HAS_PICOQUIC)
    target_compile_definitions(openmoq_publisher_lib PRIVATE OPENMOQ_HAS_PICOQUIC=1)
    if(OPENMOQ_PICOQUIC_THREAD_CHECK)
        # Activates the PICOQUIC_THREAD_DISABLE_CHECK macro in our sources;
        # picoquic itself gets the define via WITH_THREAD_CHECK above.
        target_compile_definitions(openmoq_publisher_lib PRIVATE PICOQUIC_WITH_THREAD_CHECK)
    endif()
    if(TARGET picohttp-core AND TARGET picoquic-log AND NOT APPLE AND NOT WIN32 AND CMAKE_VERSION VERSION_GREATER_EQUAL "3.24")
        # Linux with modern CMake: RESCAN resolves circular deps between picohttp/picoquic via --start-group/--end-group.
        target_link_libraries(openmoq_publisher_lib PRIVATE "$<LINK_GROUP:RESCAN,picohttp-core,picoquic-log,picoquic-core>")
        target_include_directories(openmoq_publisher_lib PRIVATE
            "${OPENMOQ_RESOLVED_PICOQUIC_SOURCE_DIR}/picohttp")
    else()
        if(NOT APPLE AND NOT WIN32)
            # GNU ld: emulate $<LINK_GROUP:RESCAN,...> for CMake < 3.24
            target_link_libraries(openmoq_publisher_lib PRIVATE
                "-Wl,--start-group" picohttp-core picoquic-log picoquic-core "-Wl,--end-group")
            target_include_directories(openmoq_publisher_lib PRIVATE
                "${OPENMOQ_RESOLVED_PICOQUIC_SOURCE_DIR}/picohttp")
        else()
            target_link_libraries(openmoq_publisher_lib PRIVATE picoquic-core)
        endif()
    endif()
    if(TARGET picohttp-core AND (APPLE OR WIN32))
        target_link_libraries(openmoq_publisher_lib PRIVATE picohttp-core)
        target_include_directories(openmoq_publisher_lib PRIVATE
            "${OPENMOQ_RESOLVED_PICOQUIC_SOURCE_DIR}/picohttp")
    endif()
    if(WIN32)
        target_link_libraries(openmoq_publisher_lib PRIVATE ws2_32 iphlpapi)
    endif()
endif()

if(OPENMOQ_HAS_SRT)
    target_compile_definitions(openmoq_publisher_lib PRIVATE OPENMOQ_HAS_SRT=1)
    target_include_directories(openmoq_publisher_lib PRIVATE ${OPENMOQ_SRT_INCLUDE_DIR})
    target_link_libraries(openmoq_publisher_lib PRIVATE ${OPENMOQ_SRT_LIBRARY})
endif()

# OPENMOQ_HAS_LIBMOQ   = libmoq source/service tier is available and links.
# OPENMOQ_ENABLE_LIBMOQ_PUBLISHER = the production Publisher routes through libmoq
#                        (only when libmoq is available AND the gate is ON).
set(OPENMOQ_ENABLE_LIBMOQ_PUBLISHER OFF)
if(OPENMOQ_HAS_LIBMOQ AND TARGET moq::service)
    target_compile_definitions(openmoq_publisher_lib PRIVATE OPENMOQ_HAS_LIBMOQ=1)
    target_link_libraries(openmoq_publisher_lib PRIVATE moq::service)
    if(OPENMOQ_USE_LIBMOQ_PUBLISHER)
        set(OPENMOQ_ENABLE_LIBMOQ_PUBLISHER ON)
        target_compile_definitions(openmoq_publisher_lib PRIVATE OPENMOQ_ENABLE_LIBMOQ_PUBLISHER=1)
    endif()
elseif(OPENMOQ_USE_LIBMOQ_PUBLISHER)
    message(WARNING
        "OPENMOQ_USE_LIBMOQ_PUBLISHER=ON but libmoq is not available; "
        "falling back to the legacy MoqtSession publish backend.")
endif()

# Migration-gate status (temporary): availability vs. selected backend.
message(STATUS "OpenMOQ: libmoq available .......... ${OPENMOQ_HAS_LIBMOQ}")
if(OPENMOQ_ENABLE_LIBMOQ_PUBLISHER)
    message(STATUS "OpenMOQ: publish backend .......... libmoq (OPENMOQ_USE_LIBMOQ_PUBLISHER=ON)")
else()
    message(STATUS "OpenMOQ: publish backend .......... legacy MoqtSession (set -DOPENMOQ_USE_LIBMOQ_PUBLISHER=ON for libmoq)")
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(openmoq_publisher_lib PRIVATE -Wall -Wextra -Wpedantic)
elseif(MSVC)
    target_compile_options(openmoq_publisher_lib PRIVATE /W4 /WX- /utf-8)
endif()

add_executable(openmoq-publisher src/main.cpp)
target_link_libraries(openmoq-publisher PRIVATE openmoq_publisher_lib)
if(OPENMOQ_ENABLE_LIBMOQ_PUBLISHER)
    # main.cpp needs to know which backend is selected to decide whether a
    # --drm-config warning for the live paths is accurate (see the
    # drm_systems live-path observability note in src/main.cpp); the define
    # is PRIVATE on openmoq_publisher_lib and does not propagate to a
    # separately-linked executable target, so it is set here too.
    target_compile_definitions(openmoq-publisher PRIVATE OPENMOQ_ENABLE_LIBMOQ_PUBLISHER=1)
endif()

include(CTest)

# Example executables (and the MSFTS example's companion test, which compiles
# example sources) are part of the default build but can be omitted entirely
# with -DOPENMOQ_BUILD_EXAMPLES=OFF. The project defines no install() rules,
# so this gate also keeps examples out of any downstream packaging that stages
# built artifacts.
if(OPENMOQ_BUILD_EXAMPLES)
    add_executable(openmoq-publisher-psychedelic-example
        examples/psychedelic/Psychedelic.cpp
    )
    target_link_libraries(openmoq-publisher-psychedelic-example PRIVATE openmoq_publisher_lib)

    add_executable(openmoq-publisher-auth-example
        examples/auth/AuthPublisher.cpp
        examples/auth/catapult_client.cpp
    )
    target_link_libraries(openmoq-publisher-auth-example PRIVATE openmoq_publisher_lib)

    add_subdirectory(examples/msfts-publisher)
else()
    message(STATUS "OpenMOQ: example executables disabled (OPENMOQ_BUILD_EXAMPLES=OFF)")
endif()

if(OPENMOQ_BUILD_TESTS)
    add_test(NAME openmoq-dependency-refresh-policy-tests
        COMMAND "${CMAKE_COMMAND}"
            -DOPENMOQ_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DOPENMOQ_TEST_TEMP_DIR=${CMAKE_CURRENT_BINARY_DIR}/dependency-refresh-test
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/dependency_refresh_test.cmake)

    add_executable(openmoq-publisher-pending-media-queue-tests
        tests/pending_media_queue_test.cpp
    )
    target_include_directories(openmoq-publisher-pending-media-queue-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/transport"
    )
    add_test(NAME openmoq-publisher-pending-media-queue-tests
        COMMAND openmoq-publisher-pending-media-queue-tests)

    add_executable(openmoq-publisher-live-media-queue-tests
        tests/live_media_queue_test.cpp
    )
    target_include_directories(openmoq-publisher-live-media-queue-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    add_test(NAME openmoq-publisher-live-media-queue-tests
        COMMAND openmoq-publisher-live-media-queue-tests)

    add_executable(openmoq-publisher-packaging-tests
        tests/cmaf_segmenter_test.cpp
    )
    target_link_libraries(openmoq-publisher-packaging-tests PRIVATE openmoq_publisher_lib)
    add_test(NAME openmoq-publisher-packaging-tests COMMAND openmoq-publisher-packaging-tests)

    add_executable(openmoq-publisher-mp4-init-tests
        tests/mp4_init_test.cpp
    )
    target_link_libraries(openmoq-publisher-mp4-init-tests PRIVATE openmoq_publisher_lib)
    add_test(NAME openmoq-publisher-mp4-init-tests COMMAND openmoq-publisher-mp4-init-tests)

    add_executable(openmoq-publisher-msf-catalog-tests
        tests/msf_catalog_test.cpp
    )
    target_link_libraries(openmoq-publisher-msf-catalog-tests PRIVATE openmoq_publisher_lib)
    add_test(NAME openmoq-publisher-msf-catalog-tests COMMAND openmoq-publisher-msf-catalog-tests)

    add_executable(openmoq-publisher-cenc-tests
        tests/cenc_test.cpp
    )
    target_link_libraries(openmoq-publisher-cenc-tests PRIVATE openmoq_publisher_lib)
    add_test(NAME openmoq-publisher-cenc-tests COMMAND openmoq-publisher-cenc-tests)

    add_executable(openmoq-publisher-msf-url-tests
        tests/msf_url_test.cpp
    )
    target_link_libraries(openmoq-publisher-msf-url-tests PRIVATE openmoq_publisher_lib)
    add_test(NAME openmoq-publisher-msf-url-tests COMMAND openmoq-publisher-msf-url-tests)

    add_executable(openmoq-publisher-cli-tests
        tests/cli_options_test.cpp
    )
    target_link_libraries(openmoq-publisher-cli-tests PRIVATE openmoq_publisher_lib)
    add_test(NAME openmoq-publisher-cli-tests COMMAND openmoq-publisher-cli-tests)

    add_executable(openmoq-publisher-endpoint-failover-tests
        tests/endpoint_failover_test.cpp
    )
    target_link_libraries(openmoq-publisher-endpoint-failover-tests PRIVATE openmoq_publisher_lib)
    add_test(NAME openmoq-publisher-endpoint-failover-tests
        COMMAND openmoq-publisher-endpoint-failover-tests)

    add_executable(openmoq-publisher-live-srt-config-tests
        tests/live_srt_config_test.cpp
    )
    target_link_libraries(openmoq-publisher-live-srt-config-tests PRIVATE openmoq_publisher_lib)
    add_test(NAME openmoq-publisher-live-srt-config-tests COMMAND openmoq-publisher-live-srt-config-tests)

    add_executable(openmoq-publisher-live-dash-tests
        tests/live_dash_ingest_test.cpp
    )
    target_link_libraries(openmoq-publisher-live-dash-tests PRIVATE openmoq_publisher_lib)
    add_test(NAME openmoq-publisher-live-dash-tests COMMAND openmoq-publisher-live-dash-tests)

    add_executable(openmoq-publisher-transport-tests
        tests/moqt_session_test.cpp
    )
    target_link_libraries(openmoq-publisher-transport-tests PRIVATE openmoq_publisher_lib)
    add_test(NAME openmoq-publisher-transport-tests COMMAND openmoq-publisher-transport-tests)

    add_executable(openmoq-publisher-webtransport-tests
        tests/webtransport_client_test.cpp
    )
    target_link_libraries(openmoq-publisher-webtransport-tests PRIVATE openmoq_publisher_lib)
    add_test(NAME openmoq-publisher-webtransport-tests COMMAND openmoq-publisher-webtransport-tests)

    add_executable(openmoq-publisher-api-tests
        tests/publisher_api_test.cpp
    )
    target_link_libraries(openmoq-publisher-api-tests PRIVATE openmoq_publisher_lib)
    if(OPENMOQ_HAS_LIBMOQ)
        target_compile_definitions(openmoq-publisher-api-tests PRIVATE OPENMOQ_HAS_LIBMOQ=1)
    endif()
    if(OPENMOQ_ENABLE_LIBMOQ_PUBLISHER)
        # Only when libmoq is the selected backend does the non-injected route go
        # through libmoq (so the test can assert its metadata gate). With the gate
        # off, non-injected publishing stays on the MoqtSession path.
        target_compile_definitions(openmoq-publisher-api-tests PRIVATE OPENMOQ_ENABLE_LIBMOQ_PUBLISHER=1)
    endif()
    add_test(NAME openmoq-publisher-api-tests COMMAND openmoq-publisher-api-tests)

    add_executable(openmoq-publisher-cat4moq-api-tests
        tests/cat4moq_api_test.cpp
    )
    target_link_libraries(openmoq-publisher-cat4moq-api-tests PRIVATE openmoq_publisher_lib)
    add_test(NAME openmoq-publisher-cat4moq-api-tests COMMAND openmoq-publisher-cat4moq-api-tests)

    add_executable(openmoq-publisher-cat4moq-transport-token-tests
        tests/cat4moq_transport_token_test.cpp
    )
    target_link_libraries(openmoq-publisher-cat4moq-transport-token-tests PRIVATE openmoq_publisher_lib)
    add_test(NAME openmoq-publisher-cat4moq-transport-token-tests COMMAND openmoq-publisher-cat4moq-transport-token-tests)

    add_executable(openmoq-publisher-control-message-tests
        tests/moqt_control_messages_test.cpp
    )
    target_link_libraries(openmoq-publisher-control-message-tests PRIVATE openmoq_publisher_lib)
    add_test(NAME openmoq-publisher-control-message-tests COMMAND openmoq-publisher-control-message-tests)

    if(OPENMOQ_HAS_LIBMOQ)
        add_executable(openmoq-publisher-libmoq-translation-tests
            tests/libmoq_translation_test.cpp
        )
        target_compile_definitions(openmoq-publisher-libmoq-translation-tests PRIVATE OPENMOQ_HAS_LIBMOQ=1)
        target_link_libraries(openmoq-publisher-libmoq-translation-tests PRIVATE openmoq_publisher_lib moq::service)
        add_test(NAME openmoq-publisher-libmoq-translation-tests COMMAND openmoq-publisher-libmoq-translation-tests)
    endif()

    if(OPENMOQ_HAS_PICOQUIC AND OPENMOQ_RUN_PICOQUIC_SMOKE_TESTS)
        add_executable(openmoq-publisher-picoquic-smoke-tests
            tests/picoquic_smoke_test.cpp
        )
        target_link_libraries(openmoq-publisher-picoquic-smoke-tests PRIVATE openmoq_publisher_lib)
        target_include_directories(openmoq-publisher-picoquic-smoke-tests PRIVATE
            "${OPENMOQ_RESOLVED_PICOQUIC_SOURCE_DIR}/picoquic"
        )
        target_compile_definitions(openmoq-publisher-picoquic-smoke-tests PRIVATE
            OPENMOQ_PICOQUIC_SOURCE_DIR="${OPENMOQ_RESOLVED_PICOQUIC_SOURCE_DIR}"
        )
        add_test(NAME openmoq-publisher-picoquic-smoke-tests COMMAND openmoq-publisher-picoquic-smoke-tests)

        add_executable(openmoq-publisher-webtransport-close-tests
            tests/webtransport_close_test.cpp
        )
        target_link_libraries(openmoq-publisher-webtransport-close-tests PRIVATE openmoq_publisher_lib picohttp-core picoquic-log)
        target_include_directories(openmoq-publisher-webtransport-close-tests PRIVATE
            "${OPENMOQ_RESOLVED_PICOQUIC_SOURCE_DIR}/picoquic"
            "${OPENMOQ_RESOLVED_PICOQUIC_SOURCE_DIR}/picohttp"
        )
        target_compile_definitions(openmoq-publisher-webtransport-close-tests PRIVATE
            OPENMOQ_PICOQUIC_SOURCE_DIR="${OPENMOQ_RESOLVED_PICOQUIC_SOURCE_DIR}"
        )
        add_test(NAME openmoq-publisher-webtransport-close-tests COMMAND openmoq-publisher-webtransport-close-tests)

        add_executable(openmoq-publisher-picoquic-close-drain-tests
            tests/picoquic_close_drain_test.cpp
        )
        target_link_libraries(openmoq-publisher-picoquic-close-drain-tests PRIVATE openmoq_publisher_lib)
        target_include_directories(openmoq-publisher-picoquic-close-drain-tests PRIVATE
            "${OPENMOQ_RESOLVED_PICOQUIC_SOURCE_DIR}/picoquic"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/transport"
        )
        target_compile_definitions(openmoq-publisher-picoquic-close-drain-tests PRIVATE
            OPENMOQ_PICOQUIC_SOURCE_DIR="${OPENMOQ_RESOLVED_PICOQUIC_SOURCE_DIR}"
        )
        add_test(NAME openmoq-publisher-picoquic-close-drain-tests COMMAND openmoq-publisher-picoquic-close-drain-tests)

        if(OPENMOQ_PICOQUIC_THREAD_CHECK)
            target_compile_definitions(openmoq-publisher-picoquic-smoke-tests
                PRIVATE PICOQUIC_WITH_THREAD_CHECK)
            target_compile_definitions(openmoq-publisher-webtransport-close-tests
                PRIVATE PICOQUIC_WITH_THREAD_CHECK)
            target_compile_definitions(openmoq-publisher-picoquic-close-drain-tests
                PRIVATE PICOQUIC_WITH_THREAD_CHECK)
        endif()

        add_executable(openmoq-publisher-picoquic-tls-verify-tests
            tests/picoquic_tls_verify_test.cpp
        )
        target_link_libraries(openmoq-publisher-picoquic-tls-verify-tests PRIVATE openmoq_publisher_lib)
        target_include_directories(openmoq-publisher-picoquic-tls-verify-tests PRIVATE
            "${OPENMOQ_RESOLVED_PICOQUIC_SOURCE_DIR}/picoquic"
        )
        target_compile_definitions(openmoq-publisher-picoquic-tls-verify-tests PRIVATE
            OPENMOQ_PICOQUIC_SOURCE_DIR="${OPENMOQ_RESOLVED_PICOQUIC_SOURCE_DIR}"
        )
        add_test(NAME openmoq-publisher-picoquic-tls-verify-tests COMMAND openmoq-publisher-picoquic-tls-verify-tests)
    endif()
endif()

if(_openmoq_managed_dependency_used)
    openmoq_record_dependency_refresh_if_due(
        REFRESH_DUE "${_openmoq_dependency_refresh_due}"
        STAMP_FILE "${_openmoq_dependency_stamp}")
endif()

# -- Installation ---------------------------------------------------------
# Only first-party artifacts are installed. The managed picotls, picoquic, and
# moq5 trees are build-time dependencies added with EXCLUDE_FROM_ALL, so none
# of their install() rules are generated. Because those dependencies are never
# installed, installation is only sound when every
# in-tree dependency in the Publisher's link closure is a static library that
# can be linked (and, for the development install, bundled) into the
# first-party artifacts; this is validated below and holds for the default
# configuration. BUILD_SHARED_LIBS=ON would turn the dependency libraries into
# shared objects living only in the build tree, so it is rejected when
# installation is requested.
#
# The development install ships a SELF-CONTAINED static archive: every
# non-imported static library in openmoq_publisher_lib's transitive link
# closure (picotls-*, picoquic-core, picoquic-log, picohttp-core, and moq5
# when enabled) is merged into the installed archive. Consumers then link only
# the genuinely external dependencies (OpenSSL, plus libsrt when
# OPENMOQ_ENABLE_SRT is ON) and MUST NOT additionally link their own
# picotls/picoquic copies, which would duplicate the embedded symbols. The
# archive is built without PIC by default; packagers who need to link it into
# shared objects should configure with -DCMAKE_POSITION_INDEPENDENT_CODE=ON.
# No install(EXPORT) package is generated on purpose: the library target
# references the excluded dependency targets, so a faithful export set cannot
# be produced until those are consumed as installed packages.
if(OPENMOQ_INSTALL)
    include(GNUInstallDirs)

    # Walk the transitive link closure of the given target and classify every
    # non-imported library target it reaches: static libraries (which the
    # development install bundles) versus anything else -- shared, module, or
    # object libraries -- which this project cannot install and therefore
    # cannot allow in the closure of an installed artifact.
    # $<LINK_ONLY:...> wrappers -- created by CMake for PRIVATE dependencies
    # of static libraries -- are unwrapped, as are the $<LINK_GROUP:...>
    # expressions this project uses for the mutually dependent picoquic
    # archives and $<LINK_LIBRARY:...>. All other generator expressions and
    # non-target entries (system libraries, imported packages such as
    # OpenSSL::Crypto, bare link flags) are deliberately treated as external.
    function(_openmoq_classify_link_closure root out_static out_nonstatic)
        set(_static "")
        set(_nonstatic "")
        set(_visited "")
        set(_pending "${root}")
        while(_pending)
            list(POP_FRONT _pending _current)
            if(_current MATCHES "^\\$<LINK_ONLY:(.+)>$")
                set(_current "${CMAKE_MATCH_1}")
            endif()
            if(_current MATCHES "^\\$<LINK_(GROUP|LIBRARY):[^,]+,(.+)>$")
                string(REPLACE "," ";" _group_members "${CMAKE_MATCH_2}")
                list(APPEND _pending ${_group_members})
                continue()
            endif()
            if(NOT TARGET "${_current}")
                continue()
            endif()
            get_target_property(_alias "${_current}" ALIASED_TARGET)
            if(_alias)
                set(_current "${_alias}")
            endif()
            if(_current IN_LIST _visited)
                continue()
            endif()
            list(APPEND _visited "${_current}")
            get_target_property(_type "${_current}" TYPE)
            get_target_property(_imported "${_current}" IMPORTED)
            if(NOT _imported)
                if(_type STREQUAL "STATIC_LIBRARY")
                    list(APPEND _static "${_current}")
                elseif(_type MATCHES "^(SHARED|MODULE|OBJECT)_LIBRARY$")
                    list(APPEND _nonstatic "${_current}")
                endif()
            endif()
            if(NOT _type STREQUAL "INTERFACE_LIBRARY")
                get_target_property(_private_deps "${_current}" LINK_LIBRARIES)
                if(_private_deps)
                    list(APPEND _pending ${_private_deps})
                endif()
            endif()
            get_target_property(_interface_deps "${_current}" INTERFACE_LINK_LIBRARIES)
            if(_interface_deps)
                list(APPEND _pending ${_interface_deps})
            endif()
        endwhile()
        set("${out_static}" "${_static}" PARENT_SCOPE)
        set("${out_nonstatic}" "${_nonstatic}" PARENT_SCOPE)
    endfunction()

    _openmoq_classify_link_closure(openmoq_publisher_lib
        _openmoq_bundled_libs _openmoq_nonstatic_libs)

    if(_openmoq_nonstatic_libs)
        message(FATAL_ERROR
            "OPENMOQ_INSTALL requires the in-tree dependency libraries to be "
            "static, but the following are not: ${_openmoq_nonstatic_libs}. "
            "This project installs only first-party artifacts, so an installed "
            "openmoq-publisher (or the bundled development archive) would "
            "reference shared libraries that exist only in the build tree. "
            "Configure with -DBUILD_SHARED_LIBS=OFF (the default), or set "
            "-DOPENMOQ_INSTALL=OFF for a build-tree-only configuration.")
    endif()

    install(TARGETS openmoq-publisher
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        COMPONENT openmoq_runtime)

    if(OPENMOQ_INSTALL_DEVELOPMENT)
        set(_openmoq_bundle_dir "${CMAKE_CURRENT_BINARY_DIR}/bundled/$<CONFIG>")
        # add_custom_command(OUTPUT) forbids target-dependent generator
        # expressions, so resolve the archive file name at configure time.
        get_target_property(_openmoq_lib_output_name openmoq_publisher_lib OUTPUT_NAME)
        if(NOT _openmoq_lib_output_name)
            set(_openmoq_lib_output_name openmoq_publisher_lib)
        endif()
        set(_openmoq_bundled_archive
            "${_openmoq_bundle_dir}/${CMAKE_STATIC_LIBRARY_PREFIX}${_openmoq_lib_output_name}${CMAKE_STATIC_LIBRARY_SUFFIX}")

        if(MSVC)
            set(_openmoq_bundle_inputs "")
            foreach(_openmoq_lib IN LISTS _openmoq_bundled_libs)
                list(APPEND _openmoq_bundle_inputs "$<TARGET_FILE:${_openmoq_lib}>")
            endforeach()
            add_custom_command(OUTPUT "${_openmoq_bundled_archive}"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${_openmoq_bundle_dir}"
                COMMAND "${CMAKE_AR}" /NOLOGO "/OUT:${_openmoq_bundled_archive}"
                    ${_openmoq_bundle_inputs}
                DEPENDS ${_openmoq_bundled_libs}
                COMMENT "Bundling static dependencies into the Publisher library"
                VERBATIM)
        elseif(APPLE)
            set(_openmoq_bundle_inputs "")
            foreach(_openmoq_lib IN LISTS _openmoq_bundled_libs)
                list(APPEND _openmoq_bundle_inputs "$<TARGET_FILE:${_openmoq_lib}>")
            endforeach()
            add_custom_command(OUTPUT "${_openmoq_bundled_archive}"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${_openmoq_bundle_dir}"
                COMMAND libtool -static -no_warning_for_no_symbols
                    -o "${_openmoq_bundled_archive}" ${_openmoq_bundle_inputs}
                DEPENDS ${_openmoq_bundled_libs}
                COMMENT "Bundling static dependencies into the Publisher library"
                VERBATIM)
        else()
            # GNU ar, llvm-ar, and FreeBSD's ELF Tool Chain ar all understand
            # MRI scripts (ar -M); merging with addlib keeps the original
            # archive members intact without extracting objects to disk, so
            # identically named members from different archives cannot
            # clobber one another. The script is fed through a small CMake
            # helper because add_custom_command() has no portable stdin
            # redirection. The MRI grammar has no quoting, hence the check
            # for spaces in the build path.
            if(CMAKE_CURRENT_BINARY_DIR MATCHES " ")
                message(FATAL_ERROR
                    "OPENMOQ_INSTALL_DEVELOPMENT merges archives with an ar "
                    "MRI script, which cannot represent paths containing "
                    "spaces. Use a build directory without spaces: "
                    "'${CMAKE_CURRENT_BINARY_DIR}'")
            endif()
            set(_openmoq_bundle_mri_file "${_openmoq_bundle_dir}/bundle.mri")
            set(_openmoq_bundle_mri "create ${_openmoq_bundled_archive}\n")
            foreach(_openmoq_lib IN LISTS _openmoq_bundled_libs)
                string(APPEND _openmoq_bundle_mri
                    "addlib $<TARGET_FILE:${_openmoq_lib}>\n")
            endforeach()
            string(APPEND _openmoq_bundle_mri "save\nend\n")
            file(GENERATE OUTPUT "${_openmoq_bundle_mri_file}"
                CONTENT "${_openmoq_bundle_mri}")

            set(_openmoq_bundle_helper
                "${CMAKE_CURRENT_BINARY_DIR}/bundled/openmoq_run_ar_mri.cmake")
            file(WRITE "${_openmoq_bundle_helper}"
"execute_process(
    COMMAND \"\${AR}\" -M
    INPUT_FILE \"\${MRI}\"
    RESULT_VARIABLE _openmoq_ar_result)
if(NOT _openmoq_ar_result EQUAL 0)
    message(FATAL_ERROR
        \"Bundling static Publisher library failed: \"
        \"\${AR} -M < \${MRI} (exit \${_openmoq_ar_result})\")
endif()
")
            add_custom_command(OUTPUT "${_openmoq_bundled_archive}"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${_openmoq_bundle_dir}"
                COMMAND "${CMAKE_COMMAND}"
                    "-DAR=${CMAKE_AR}"
                    "-DMRI=${_openmoq_bundle_mri_file}"
                    -P "${_openmoq_bundle_helper}"
                DEPENDS ${_openmoq_bundled_libs} "${_openmoq_bundle_mri_file}"
                COMMENT "Bundling static dependencies into the Publisher library"
                VERBATIM)
        endif()

        add_custom_target(openmoq_publisher_bundled ALL
            DEPENDS "${_openmoq_bundled_archive}")

        install(FILES "${_openmoq_bundled_archive}"
            DESTINATION "${CMAKE_INSTALL_LIBDIR}"
            COMPONENT openmoq_development)

        # Public Publisher API headers (include/openmoq/publisher/...).
        install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/openmoq"
            DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
            COMPONENT openmoq_development
            FILES_MATCHING PATTERN "*.h")
    endif()
endif()
