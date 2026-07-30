cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR
        "In-source builds are not supported. Configure with: cmake -S . -B build")
endif()

project(openmoq_publisher
    VERSION 0.1.0
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

# Resolve default picoquic/picotls source dirs, preferring (in order): an
# existing sibling checkout next to moqxr (../picoquic, ../picotls), then
# third_party/, then thirdparty/. The first candidate that contains a
# CMakeLists.txt wins; otherwise fall back to the third_party path.
function(_openmoq_pick_dep_default out_var name)
    foreach(_candidate
            "${CMAKE_CURRENT_SOURCE_DIR}/../${name}"
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/${name}"
            "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/${name}")
        if(EXISTS "${_candidate}/CMakeLists.txt")
            get_filename_component(_abs "${_candidate}" ABSOLUTE)
            set(${out_var} "${_abs}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${out_var} "${CMAKE_CURRENT_SOURCE_DIR}/third_party/${name}" PARENT_SCOPE)
endfunction()

_openmoq_pick_dep_default(_OPENMOQ_DEFAULT_PICOQUIC_SOURCE_DIR picoquic)
_openmoq_pick_dep_default(_OPENMOQ_DEFAULT_PICOTLS_SOURCE_DIR picotls)

set(OPENMOQ_PICOQUIC_SOURCE_DIR "${_OPENMOQ_DEFAULT_PICOQUIC_SOURCE_DIR}" CACHE PATH
    "Path to a picoquic source checkout (defaults to ../picoquic, then third_party/, then thirdparty/)")
set(OPENMOQ_PICOTLS_SOURCE_DIR "${_OPENMOQ_DEFAULT_PICOTLS_SOURCE_DIR}" CACHE PATH
    "Path to a picotls source checkout (defaults to ../picotls, then third_party/, then thirdparty/)")

function(_openmoq_pick_libmoq_default out_var)
    foreach(_candidate
            "${CMAKE_CURRENT_SOURCE_DIR}/../moq5"
            "${CMAKE_CURRENT_SOURCE_DIR}/../libmoq"
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/moq5"
            "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/moq5")
        if(EXISTS "${_candidate}/CMakeLists.txt")
            get_filename_component(_abs "${_candidate}" ABSOLUTE)
            set(${out_var} "${_abs}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${out_var} "${CMAKE_CURRENT_SOURCE_DIR}/../moq5" PARENT_SCOPE)
endfunction()

_openmoq_pick_libmoq_default(_OPENMOQ_DEFAULT_LIBMOQ_SOURCE_DIR)

# OpenSSL coherency: the picotls checkout here uses 1.1.x-only APIs
# (EVP_PKEY_*_tls_encodedpoint), so pin ONE OpenSSL 1.1 install for both headers
# AND libraries. Without this, CMake's FindOpenSSL on macOS can mix
# GStreamer.framework headers with a Homebrew openssl@3 libcrypto, which lacks
# those symbols -> undefined-symbol link failures in libpicotls-openssl.
set(OPENMOQ_OPENSSL_ROOT_DIR "" CACHE PATH
    "OpenSSL prefix for picoquic/picotls/libmoq (auto: Homebrew openssl@1.1 if found)")
if(NOT OPENMOQ_OPENSSL_ROOT_DIR)
    foreach(_ssl_prefix
            "/opt/homebrew/opt/openssl@1.1"
            "/usr/local/opt/openssl@1.1")
        if(EXISTS "${_ssl_prefix}/include/openssl/ssl.h")
            set(OPENMOQ_OPENSSL_ROOT_DIR "${_ssl_prefix}" CACHE PATH "" FORCE)
            break()
        endif()
    endforeach()
endif()
if(OPENMOQ_OPENSSL_ROOT_DIR)
    # Cache + FORCE so the picoquic/picotls subtree's find_package(OpenSSL) and
    # libmoq all resolve the same prefix; FIND_FRAMEWORK LAST keeps Homebrew
    # headers ahead of /Library/Frameworks/*.
    set(OPENSSL_ROOT_DIR "${OPENMOQ_OPENSSL_ROOT_DIR}" CACHE PATH "" FORCE)
    set(CMAKE_FIND_FRAMEWORK LAST)
    message(STATUS "OpenMOQ: pinning OpenSSL to ${OPENMOQ_OPENSSL_ROOT_DIR}")
else()
    message(STATUS "OpenMOQ: no pinned OpenSSL found; relying on CMake default FindOpenSSL")
endif()

set(OPENMOQ_HAS_PICOQUIC OFF)

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

if(OPENMOQ_ENABLE_PICOQUIC AND EXISTS "${OPENMOQ_PICOQUIC_SOURCE_DIR}/CMakeLists.txt")
    if(EXISTS "${OPENMOQ_PICOTLS_SOURCE_DIR}/CMakeLists.txt")
        # picotls still declares CMake 2.8 compatibility; newer CMake releases
        # require an explicit policy floor when configuring that subproject.
        set(CMAKE_POLICY_VERSION_MINIMUM "3.5" CACHE STRING "" FORCE)
        set(PICOQUIC_FETCH_PTLS ON CACHE BOOL "" FORCE)
        set(FETCHCONTENT_SOURCE_DIR_PICOTLS "${OPENMOQ_PICOTLS_SOURCE_DIR}" CACHE PATH "" FORCE)
        set(picoquic_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(BUILD_DEMO OFF CACHE BOOL "" FORCE)
        set(BUILD_HTTP ON CACHE BOOL "" FORCE)
        set(BUILD_PQBENCH OFF CACHE BOOL "" FORCE)
        set(BUILD_LOGLIB ON CACHE BOOL "" FORCE)
        set(BUILD_LOGREADER OFF CACHE BOOL "" FORCE)
        if(WIN32 AND EXISTS "${OPENMOQ_PICOTLS_SOURCE_DIR}/picotlsvs/picotls")
            # picotls.h includes "wincompat.h" on _WINDOWS but picotls's own CMakeLists.txt
            # does not add picotlsvs/picotls to any include path. Setting it globally here
            # covers picotls-core, picotls-openssl, picotls-fusion, and picotls-minicrypto,
            # all of which compile sources that transitively include picotls.h.
            include_directories(BEFORE "${OPENMOQ_PICOTLS_SOURCE_DIR}/picotlsvs/picotls")
        endif()
        if(NOT WIN32)
            # picotls appends literal "(" and ")" list items when Brotli is
            # discovered via pkg-config. On macOS, CMake rejects the resulting
            # malformed source-prefixed link directories during generate. Scope
            # pkg-config away from system package metadata while picotls configures.
            set(_openmoq_saved_pkg_config_libdir "$ENV{PKG_CONFIG_LIBDIR}")
            set(_openmoq_saved_pkg_config_path "$ENV{PKG_CONFIG_PATH}")
            set(ENV{PKG_CONFIG_LIBDIR} "${CMAKE_CURRENT_BINARY_DIR}/openmoq-empty-pkgconfig")
            set(ENV{PKG_CONFIG_PATH} "${CMAKE_CURRENT_BINARY_DIR}/openmoq-empty-pkgconfig")
            file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/openmoq-empty-pkgconfig")
        endif()
        add_subdirectory("${OPENMOQ_PICOQUIC_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/picoquic")
        if(NOT WIN32)
            set(ENV{PKG_CONFIG_LIBDIR} "${_openmoq_saved_pkg_config_libdir}")
            set(ENV{PKG_CONFIG_PATH} "${_openmoq_saved_pkg_config_path}")
            unset(_openmoq_saved_pkg_config_libdir)
            unset(_openmoq_saved_pkg_config_path)
        endif()
        # picotls currently injects literal "(" and ")" entries into its link
        # directories list when Brotli is discovered via pkg-config. CMake on
        # macOS rejects those malformed source-prefixed paths during generate.
        openmoq_sanitize_target_link_directories(picotls-core)
        if(WIN32)
            # picotls's cli, bench, and test executables use POSIX headers not available
            # on Windows. Exclude them from the default build; the libraries we need
            # (picotls-core, picotls-openssl, picotls-fusion) are unaffected.
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
            # picotls-core calls wintimeofday() which is only compiled inside the VS
            # project files; add it explicitly for CMake Windows builds.
            if(TARGET picotls-core AND EXISTS "${OPENMOQ_PICOTLS_SOURCE_DIR}/picotlsvs/picotls/wintimeofday.c")
                target_sources(picotls-core PRIVATE
                    "${OPENMOQ_PICOTLS_SOURCE_DIR}/picotlsvs/picotls/wintimeofday.c")
            endif()
            # picotls-minicrypto uses the Windows BCrypt API for random bytes.
            if(TARGET picotls-minicrypto)
                target_link_libraries(picotls-minicrypto bcrypt)
            endif()
        endif()
        set(OPENMOQ_HAS_PICOQUIC ON)
    else()
        message(STATUS "picoquic checkout found, but picotls source was not found; building without picoquic integration")
    endif()
endif()

# -- libmoq integration -------------------------------------------------
# Build moqxr against the sibling libmoq service tier (moq::service). This is
# added AFTER picoquic above on purpose: libmoq's find_package(Picoquic) uses
# "already-defined target" as its first resolution step, so it reuses the
# picoquic-core target moqxr already created instead of add_subdirectory'ing
# picoquic a second time (which would collide on target names). By default moqxr
# still publishes via its own (legacy) transport; the Publisher only routes
# through libmoq when -DOPENMOQ_USE_LIBMOQ_PUBLISHER=ON. Linking libmoq always
# validates that the service tier builds and links into this project.
set(OPENMOQ_LIBMOQ_SOURCE_DIR "${_OPENMOQ_DEFAULT_LIBMOQ_SOURCE_DIR}" CACHE PATH
    "Path to the moq5/libmoq source checkout (defaults to ../moq5, then ../libmoq, third_party/moq5, or thirdparty/moq5)")

set(OPENMOQ_HAS_LIBMOQ OFF)
if(EXISTS "${OPENMOQ_LIBMOQ_SOURCE_DIR}/CMakeLists.txt")
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
        set(MOQ_PICOQUIC_SOURCE_DIR "${OPENMOQ_PICOQUIC_SOURCE_DIR}" CACHE PATH "" FORCE)
        if(NOT MOQ_PICOTLS_PREFIX)
            set(MOQ_PICOTLS_PREFIX "${OPENMOQ_PICOTLS_SOURCE_DIR}" CACHE PATH "" FORCE)
        endif()
    else()
        message(STATUS "libmoq: picoquic not available; building core/service tier without the raw QUIC adapter")
    endif()

    add_subdirectory("${OPENMOQ_LIBMOQ_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/libmoq" EXCLUDE_FROM_ALL)
    set(OPENMOQ_HAS_LIBMOQ ON)
else()
    message(STATUS "libmoq source not found at ${OPENMOQ_LIBMOQ_SOURCE_DIR}; building without libmoq integration")
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
    src/mp4_box.cpp
    src/publisher_api.cpp
    src/transport/libmoq_publisher.cpp
    src/transport/moqt_control_messages.cpp
    src/transport/moqt_session.cpp
    src/transport/picoquic_client.cpp
    src/transport/publisher_transport.cpp
    src/transport/webtransport_client.cpp
)

set_target_properties(openmoq_publisher_lib PROPERTIES
    OUTPUT_NAME openmoq_publisher
)

target_include_directories(openmoq_publisher_lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(openmoq_publisher_lib PUBLIC cxx_std_20)

if(OPENMOQ_HAS_PICOQUIC)
    target_compile_definitions(openmoq_publisher_lib PRIVATE OPENMOQ_HAS_PICOQUIC=1)
    if(TARGET picohttp-core AND TARGET picoquic-log AND NOT APPLE AND NOT WIN32 AND CMAKE_VERSION VERSION_GREATER_EQUAL "3.24")
        # Linux with modern CMake: RESCAN resolves circular deps between picohttp/picoquic via --start-group/--end-group.
        target_link_libraries(openmoq_publisher_lib PRIVATE "$<LINK_GROUP:RESCAN,picohttp-core,picoquic-log,picoquic-core>")
        target_include_directories(openmoq_publisher_lib PRIVATE "${OPENMOQ_PICOQUIC_SOURCE_DIR}/picohttp")
    else()
        target_link_libraries(openmoq_publisher_lib PRIVATE picoquic-core)
    endif()
    if(TARGET picohttp-core AND (APPLE OR WIN32 OR CMAKE_VERSION VERSION_LESS "3.24"))
        target_link_libraries(openmoq_publisher_lib PRIVATE picohttp-core)
        target_include_directories(openmoq_publisher_lib PRIVATE "${OPENMOQ_PICOQUIC_SOURCE_DIR}/picohttp")
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
    add_executable(openmoq-publisher-packaging-tests
        tests/cmaf_segmenter_test.cpp
    )
    target_link_libraries(openmoq-publisher-packaging-tests PRIVATE openmoq_publisher_lib)
    add_test(NAME openmoq-publisher-packaging-tests COMMAND openmoq-publisher-packaging-tests)

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
            "${OPENMOQ_PICOQUIC_SOURCE_DIR}/picoquic"
        )
        target_compile_definitions(openmoq-publisher-picoquic-smoke-tests PRIVATE
            OPENMOQ_PICOQUIC_SOURCE_DIR="${OPENMOQ_PICOQUIC_SOURCE_DIR}"
        )
        add_test(NAME openmoq-publisher-picoquic-smoke-tests COMMAND openmoq-publisher-picoquic-smoke-tests)
    endif()
endif()
