include_guard(GLOBAL)

function(openmoq_dependency_refresh_due)
    set(options)
    set(one_value_args STAMP_FILE INTERVAL_HOURS NOW_EPOCH RESULT)
    cmake_parse_arguments(ARG "${options}" "${one_value_args}" "" ${ARGN})

    foreach(required_arg IN ITEMS STAMP_FILE INTERVAL_HOURS RESULT)
        if(NOT DEFINED ARG_${required_arg} OR "${ARG_${required_arg}}" STREQUAL "")
            message(FATAL_ERROR
                "openmoq_dependency_refresh_due requires ${required_arg}")
        endif()
    endforeach()

    if(NOT ARG_INTERVAL_HOURS MATCHES "^[0-9]+$")
        message(FATAL_ERROR "INTERVAL_HOURS must be a non-negative integer")
    endif()

    if(DEFINED ARG_NOW_EPOCH AND NOT "${ARG_NOW_EPOCH}" STREQUAL "")
        set(now_epoch "${ARG_NOW_EPOCH}")
    else()
        string(TIMESTAMP now_epoch "%s" UTC)
    endif()

    set(refresh_due TRUE)
    if(EXISTS "${ARG_STAMP_FILE}")
        file(READ "${ARG_STAMP_FILE}" last_refresh_epoch)
        string(STRIP "${last_refresh_epoch}" last_refresh_epoch)
        if(last_refresh_epoch MATCHES "^[0-9]+$")
            math(EXPR refresh_interval_seconds "${ARG_INTERVAL_HOURS} * 60 * 60")
            math(EXPR refresh_age_seconds "${now_epoch} - ${last_refresh_epoch}")
            if(refresh_age_seconds GREATER_EQUAL 0 AND
                    refresh_age_seconds LESS refresh_interval_seconds)
                set(refresh_due FALSE)
            endif()
        endif()
    endif()

    set(${ARG_RESULT} "${refresh_due}" PARENT_SCOPE)
endfunction()

function(openmoq_record_dependency_refresh)
    set(options)
    set(one_value_args STAMP_FILE NOW_EPOCH)
    cmake_parse_arguments(ARG "${options}" "${one_value_args}" "" ${ARGN})

    if(NOT DEFINED ARG_STAMP_FILE OR "${ARG_STAMP_FILE}" STREQUAL "")
        message(FATAL_ERROR "openmoq_record_dependency_refresh requires STAMP_FILE")
    endif()

    if(DEFINED ARG_NOW_EPOCH AND NOT "${ARG_NOW_EPOCH}" STREQUAL "")
        set(now_epoch "${ARG_NOW_EPOCH}")
    else()
        string(TIMESTAMP now_epoch "%s" UTC)
    endif()

    get_filename_component(stamp_directory "${ARG_STAMP_FILE}" DIRECTORY)
    file(MAKE_DIRECTORY "${stamp_directory}")
    file(WRITE "${ARG_STAMP_FILE}" "${now_epoch}\n")
endfunction()

function(openmoq_record_dependency_refresh_if_due)
    set(options)
    set(one_value_args REFRESH_DUE STAMP_FILE NOW_EPOCH)
    cmake_parse_arguments(ARG "${options}" "${one_value_args}" "" ${ARGN})

    if(NOT DEFINED ARG_REFRESH_DUE)
        message(FATAL_ERROR
            "openmoq_record_dependency_refresh_if_due requires REFRESH_DUE")
    endif()
    if(NOT DEFINED ARG_STAMP_FILE OR "${ARG_STAMP_FILE}" STREQUAL "")
        message(FATAL_ERROR
            "openmoq_record_dependency_refresh_if_due requires STAMP_FILE")
    endif()

    if(ARG_REFRESH_DUE)
        openmoq_record_dependency_refresh(
            STAMP_FILE "${ARG_STAMP_FILE}"
            NOW_EPOCH "${ARG_NOW_EPOCH}")
    endif()
endfunction()

function(openmoq_report_git_dependency dependency_name source_dir tracking_ref)
    find_package(Git QUIET)
    if(NOT Git_FOUND OR NOT EXISTS "${source_dir}/.git")
        message(STATUS
            "OpenMOQ: ${dependency_name} source=${source_dir} ref=${tracking_ref}")
        return()
    endif()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
        WORKING_DIRECTORY "${source_dir}"
        RESULT_VARIABLE git_result
        OUTPUT_VARIABLE git_head
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(git_result EQUAL 0)
        message(STATUS
            "OpenMOQ: ${dependency_name} source=${source_dir} "
            "ref=${tracking_ref} commit=${git_head}")
    else()
        message(STATUS
            "OpenMOQ: ${dependency_name} source=${source_dir} ref=${tracking_ref}")
    endif()
endfunction()
