add_executable(kvv_core_tests test_core_helpers.cpp)

target_compile_definitions(kvv_core_tests PRIVATE KVV_TRACKER_UNIT_TEST)

target_link_libraries(kvv_core_tests PRIVATE
    Catch2::Catch2WithMain
    Crow::Crow
    nlohmann_json::nlohmann_json
    cpr::cpr
    PostgreSQL::PostgreSQL
    OpenSSL::Crypto
)

add_test(NAME kvv_core_tests COMMAND kvv_core_tests)
