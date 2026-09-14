include_guard(GLOBAL)

function(openmoq_adapt_picoquic_save_ticket picoquic_source picotls_source)
    set(tls_source "${picoquic_source}/picoquic/tls_api.c")
    set(tls_header "${picotls_source}/include/picotls.h")
    file(READ "${tls_header}" header)
    file(READ "${tls_source}" source)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${tls_header}" "${tls_source}")

    # picotls added ticket metadata before picoquic adopted the new callback ABI.
    # Compile an adapted copy only for that combination; never edit source overrides.
    set(old_callback "int picoquic_client_save_ticket_call_back(ptls_save_ticket_t* save_ticket_ctx,\n    ptls_t* tls, ptls_iovec_t input)\n{")
    string(FIND "${source}" "${old_callback}" old_callback_position)
    if(NOT header MATCHES "PTLS_CALLBACK_TYPE\\(int, save_ticket,[^;]*ptls_save_ticket_properties_t" OR
            old_callback_position EQUAL -1)
        return()
    endif()

    string(REPLACE "${old_callback}"
        "int picoquic_client_save_ticket_call_back(ptls_save_ticket_t* save_ticket_ctx,\n    ptls_t* tls, ptls_iovec_t input, const ptls_save_ticket_properties_t* properties)\n{\n    (void)properties;"
        source "${source}")
    set(adapted_source "${CMAKE_CURRENT_BINARY_DIR}/picoquic-compat/tls_api.c")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/picoquic-compat")
    # Avoid rebuilding this translation unit on every configure.
    file(CONFIGURE OUTPUT "${adapted_source}" CONTENT "@source@" @ONLY)

    get_target_property(sources picoquic-core SOURCES)
    get_target_property(source_directory picoquic-core SOURCE_DIR)
    set(adapted_sources)
    set(replaced FALSE)
    foreach(entry IN LISTS sources)
        get_filename_component(absolute_entry "${entry}" ABSOLUTE BASE_DIR "${source_directory}")
        if(absolute_entry STREQUAL tls_source)
            list(APPEND adapted_sources "${adapted_source}")
            set(replaced TRUE)
        else()
            list(APPEND adapted_sources "${entry}")
        endif()
    endforeach()
    if(NOT replaced)
        message(FATAL_ERROR "Cannot locate picoquic tls_api.c for save-ticket compatibility")
    endif()
    set_property(TARGET picoquic-core PROPERTY SOURCES "${adapted_sources}")
    message(STATUS "OpenMOQ: adapting picoquic save-ticket callback to picotls ticket metadata ABI")
endfunction()
