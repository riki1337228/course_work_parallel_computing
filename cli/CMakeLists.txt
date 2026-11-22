add_executable(search_cli main.cpp)
set_target_properties(search_cli PROPERTIES OUTPUT_NAME search)
target_link_libraries(search_cli PRIVATE Drogon::Drogon nlohmann_json::nlohmann_json)
