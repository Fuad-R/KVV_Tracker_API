cmake_minimum_required(VERSION 3.14)

project(KVVAggregatorTests)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Fetch Catch2 ---
include(FetchContent)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.5.2
)
FetchContent_MakeAvailable(Catch2)

# --- Fetch nlohmann/json (same version as main project) ---
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(json)

# --- OpenSSL ---
find_package(OpenSSL REQUIRED)

# --- Test executable ---
add_executable(unit_tests test_utils.cpp)

target_include_directories(unit_tests PRIVATE ${CMAKE_SOURCE_DIR}/../..)

target_link_libraries(unit_tests PRIVATE
    Catch2::Catch2WithMain
    nlohmann_json::nlohmann_json
    OpenSSL::Crypto
)

# --- Register with CTest ---
include(CTest)
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(unit_tests)
