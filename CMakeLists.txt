cmake_minimum_required(VERSION 3.14)

# FIX: Set policy to handle older dependencies gracefully in CMake 4.0+
if(POLICY CMP0048)
    cmake_policy(SET CMP0048 NEW)
endif()

project(KVVAggregator)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# ------------------------------------------------------------------------------
# 1. Fetch Asio (Standalone) - REQUIRED before Crow
# ------------------------------------------------------------------------------
FetchContent_Declare(
        asio
        GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
        GIT_TAG asio-1-30-2
)
FetchContent_MakeAvailable(asio)

# Pre-set the variable so Crow finds it immediately
set(ASIO_INCLUDE_DIR ${asio_SOURCE_DIR}/asio/include CACHE PATH "Asio include dir" FORCE)
include_directories(${ASIO_INCLUDE_DIR})
add_definitions(-DASIO_STANDALONE)

# ------------------------------------------------------------------------------
# 2. Fetch Crow
# ------------------------------------------------------------------------------
FetchContent_Declare(
        Crow
        GIT_REPOSITORY https://github.com/CrowCpp/Crow.git
        GIT_TAG master
)
set(CROW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CROW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CROW_USE_BOOST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(Crow)

# ------------------------------------------------------------------------------
# 3. Fetch nlohmann/json (Updated Version)
# ------------------------------------------------------------------------------
FetchContent_Declare(
        json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3  # BUMPED VERSION to fix CMake < 3.5 error
)
# Option to skip JSON's internal CMake setup if it continues to fail
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(json)

# ------------------------------------------------------------------------------
# 4. Fetch cpr (HTTP Client)
# ------------------------------------------------------------------------------
FetchContent_Declare(
        cpr
        GIT_REPOSITORY https://github.com/libcpr/cpr.git
        GIT_TAG 1.10.5
)
set(CPR_USE_SYSTEM_CURL OFF CACHE BOOL "" FORCE)
set(BUILD_CPR_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(cpr)

# ------------------------------------------------------------------------------
# 5. Build Target
# ------------------------------------------------------------------------------
add_executable(kvv_aggregator main.cpp)

target_link_libraries(kvv_aggregator PRIVATE
        Crow::Crow
        nlohmann_json::nlohmann_json
        cpr::cpr
)
