cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR
        "In-source builds are not supported. Configure with: cmake -S . -B build")
endif()

project(openmoq_publisher
    VERSION 0.3.14
    DESCRIPTION "OpenMOQ contribution project: C++20 publisher for fragmented MP4 to CMSF/MOQT"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(OPENMOQ_BUILD_TESTS "Build OpenMOQ publisher tests" ON)
option(OPENMOQ_ENABLE_PICOQUIC "Enable picoquic transport integration when picoquic is available" ON)
option(OPENMOQ_RUN_PICOQUIC_SMOKE_TESTS "Build and run picoquic loopback smoke tests" OFF)
option(OPENMOQ_ENABLE_SRT "Enable libsrt ingest support when libsrt is available" ON)
# Temporary migration gate: route the production Publisher through libmoq instead
# of the legacy MoqtSession path. Requires libmoq to be available. OFF keeps the
# old/moxygen publish path as the default until the migration is accepted; the
# libmoq integration code still builds and is tested whenever libmoq is present.
option(OPENMOQ_USE_LIBMOQ_PUBLISHER "Use libmoq as the production publish backend (requires libmoq)" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(OpenMoqDependencies)
include(OpenMoqVersion)
include(FetchContent)

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
            SOURCE_DIR "${OPENMOQ_RESOLVED_PICOTLS_SOURCE_DIR}")
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
            SOURCE_DIR "${OPENMOQ_RESOLVED_PICOTLS_SOURCE_DIR}")
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
    FetchContent_MakeAvailable(picotls)
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
        add_subdirectory("${OPENMOQ_RESOLVED_PICOQUIC_SOURCE_DIR}"
            "${CMAKE_CURRENT_BINARY_DIR}/picoquic")
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
            SOURCE_DIR "${OPENMOQ_RESOLVED_PICOQUIC_SOURCE_DIR}")
        FetchContent_MakeAvailable(picoquic)
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

add_executable(openmoq-publisher-psychedelic-example
    examples/psychedelic/Psychedelic.cpp
)
target_link_libraries(openmoq-publisher-psychedelic-example PRIVATE openmoq_publisher_lib)

add_executable(openmoq-publisher-auth-example
    examples/auth/AuthPublisher.cpp
    examples/auth/catapult_client.cpp
)
target_link_libraries(openmoq-publisher-auth-example PRIVATE openmoq_publisher_lib)

include(CTest)

add_subdirectory(examples/msfts-publisher)

if(OPENMOQ_BUILD_TESTS)
    add_test(NAME openmoq-dependency-refresh-policy-tests
        COMMAND "${CMAKE_COMMAND}"
            -DOPENMOQ_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DOPENMOQ_TEST_TEMP_DIR=${CMAKE_CURRENT_BINARY_DIR}/dependency-refresh-test
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/dependency_refresh_test.cmake)

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
        )
        target_compile_definitions(openmoq-publisher-picoquic-close-drain-tests PRIVATE
            OPENMOQ_PICOQUIC_SOURCE_DIR="${OPENMOQ_RESOLVED_PICOQUIC_SOURCE_DIR}"
        )
        add_test(NAME openmoq-publisher-picoquic-close-drain-tests COMMAND openmoq-publisher-picoquic-close-drain-tests)

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
