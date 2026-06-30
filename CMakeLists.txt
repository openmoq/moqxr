cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR
        "In-source builds are not supported. Configure with: cmake -S . -B build")
endif()

project(openmoq_publisher
    VERSION 0.1.0
    DESCRIPTION "OpenMOQ contribution project: C++20 publisher for fragmented MP4 to CMSF/MOQT"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(OPENMOQ_BUILD_TESTS "Build OpenMOQ publisher tests" ON)
option(OPENMOQ_ENABLE_PICOQUIC "Enable picoquic transport integration when picoquic is available" ON)
option(OPENMOQ_RUN_PICOQUIC_SMOKE_TESTS "Build and run picoquic loopback smoke tests" OFF)
option(OPENMOQ_ENABLE_SRT "Enable libsrt ingest support when libsrt is available" ON)

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

set(_OPENMOQ_THIRDPARTY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
if(NOT EXISTS "${_OPENMOQ_THIRDPARTY_ROOT}")
    set(_OPENMOQ_THIRDPARTY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty")
endif()

set(_OPENMOQ_DEFAULT_PICOQUIC_SOURCE_DIR "${_OPENMOQ_THIRDPARTY_ROOT}/picoquic")
set(_OPENMOQ_DEFAULT_PICOTLS_SOURCE_DIR "${_OPENMOQ_THIRDPARTY_ROOT}/picotls")

set(OPENMOQ_PICOQUIC_SOURCE_DIR "${_OPENMOQ_DEFAULT_PICOQUIC_SOURCE_DIR}" CACHE PATH
    "Path to a picoquic source checkout (defaults to third_party/ or thirdparty/)")
set(OPENMOQ_PICOTLS_SOURCE_DIR "${_OPENMOQ_DEFAULT_PICOTLS_SOURCE_DIR}" CACHE PATH
    "Path to a picotls source checkout (defaults to third_party/ or thirdparty/)")

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

add_library(openmoq_publisher_lib STATIC
    src/cat4moq.cpp
    src/cli_options.cpp
    src/cmaf_segmenter.cpp
    src/cmsf_packager.cpp
    src/live_srt_ingest.cpp
    src/live_srt_config.cpp
    src/moq_draft.cpp
    src/mp4_box.cpp
    src/publisher_api.cpp
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

if(OPENMOQ_BUILD_TESTS)
    add_executable(openmoq-publisher-packaging-tests
        tests/cmaf_segmenter_test.cpp
    )
    target_link_libraries(openmoq-publisher-packaging-tests PRIVATE openmoq_publisher_lib)
    add_test(NAME openmoq-publisher-packaging-tests COMMAND openmoq-publisher-packaging-tests)

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

    if(OPENMOQ_HAS_PICOQUIC AND OPENMOQ_RUN_PICOQUIC_SMOKE_TESTS)
        add_executable(openmoq-publisher-picoquic-smoke-tests
            tests/picoquic_smoke_test.cpp
        )
        target_link_libraries(openmoq-publisher-picoquic-smoke-tests PRIVATE openmoq_publisher_lib)
        target_include_directories(openmoq-publisher-picoquic-smoke-tests PRIVATE
            "${OPENMOQ_PICOQUIC_SOURCE_DIR}/picoquic"
        )
        add_test(NAME openmoq-publisher-picoquic-smoke-tests COMMAND openmoq-publisher-picoquic-smoke-tests)
    endif()
endif()
