# ---------------------------------------------------------------------------
# search_core: framework-independent core (text processing, thread pool,
# inverted index, ranking, query expansion logic, service orchestration).
# It has no Drogon dependency, so it can be unit-tested and built with TSan.
# ---------------------------------------------------------------------------
add_library(search_core STATIC
    src/core/Config.cpp
    src/core/TextProcessor.cpp
    src/core/DocumentLoader.cpp
    src/threading/ThreadPool.cpp
    src/index/InvertedIndex.cpp
    src/index/IndexBuilder.cpp
    src/search/SearchEngine.cpp
    src/search/SearchService.cpp
    src/ollama/OllamaProtocol.cpp
    src/ollama/QueryExpander.cpp
)
target_include_directories(search_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(search_core PUBLIC Threads::Threads nlohmann_json::nlohmann_json)

if(BUILD_API)
    # -----------------------------------------------------------------------
    # search_api: HTTP/WebSocket layer and Ollama HTTP client (Drogon).
    # OBJECT library so that Drogon's self-registering WebSocket controller
    # is never dropped by the linker.
    # -----------------------------------------------------------------------
    add_library(search_api OBJECT
        src/ollama/OllamaClient.cpp
        src/api/JsonResponse.cpp
        src/api/HealthController.cpp
        src/api/IndexController.cpp
        src/api/SearchController.cpp
        src/api/StatsController.cpp
        src/api/SearchWebSocket.cpp
        src/api/Routes.cpp
    )
    target_include_directories(search_api PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(search_api PUBLIC search_core Drogon::Drogon)

    add_executable(search_server src/main.cpp)
    target_link_libraries(search_server PRIVATE search_api)
endif()
