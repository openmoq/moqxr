add_executable(openmoq-publisher-msfts-example
    main.cpp
    msfts_options.cpp
    msfts_source.cpp
)
target_link_libraries(openmoq-publisher-msfts-example PRIVATE openmoq_publisher_lib)

if(OPENMOQ_BUILD_TESTS)
    add_executable(openmoq-publisher-msfts-tests
        tests/msfts_source_test.cpp
        msfts_options.cpp
        msfts_source.cpp
    )
    target_link_libraries(openmoq-publisher-msfts-tests PRIVATE openmoq_publisher_lib)
    add_test(NAME openmoq-publisher-msfts-tests COMMAND openmoq-publisher-msfts-tests)
    set_tests_properties(openmoq-publisher-msfts-tests PROPERTIES TIMEOUT 60)
endif()
