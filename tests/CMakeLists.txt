find_package(GTest REQUIRED)
include(GoogleTest)

# ---------------------------------------------------------------- unit + concurrency
add_executable(unit_tests
    unit/TestUtils.hpp
    unit/ThreadPoolTest.cpp
    unit/TextProcessorTest.cpp
    unit/DocumentLoaderTest.cpp
    unit/InvertedIndexTest.cpp
    unit/IndexBuilderTest.cpp
    unit/SearchEngineTest.cpp
    unit/QueryExpanderTest.cpp
    unit/OllamaProtocolTest.cpp
    unit/SearchServiceTest.cpp
    unit/ConcurrencyTest.cpp
    unit/ConfigTest.cpp
)
target_link_libraries(unit_tests PRIVATE search_core GTest::gtest GTest::gtest_main)
gtest_discover_tests(unit_tests DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 300)

# ---------------------------------------------------------------- integration (HTTP + WebSocket)
if(BUILD_API)
    add_executable(integration_tests
        integration/ApiIntegrationTest.cpp
    )
    target_link_libraries(integration_tests PRIVATE search_api GTest::gtest)
    # A single process hosts the Drogon app, so the suite is registered as one test.
    add_test(NAME integration_tests COMMAND integration_tests)
    set_tests_properties(integration_tests PROPERTIES TIMEOUT 600)
endif()
