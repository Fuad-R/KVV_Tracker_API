cmake_minimum_required(VERSION 3.14)

if(POLICY CMP0048)
    cmake_policy(SET CMP0048 NEW)
endif()

project(KVVAggregator)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)

option(ENABLE_COVERAGE "Enable coverage instrumentation for GNU/Clang builds" OFF)
option(TRANSIT_TRACKER_BUILD_TESTS "Build Transit Tracker unit tests" ON)

include(CTest)
include(FetchContent)

FetchContent_Declare(
    asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG asio-1-30-2
)
FetchContent_MakeAvailable(asio)

set(ASIO_INCLUDE_DIR ${asio_SOURCE_DIR}/asio/include CACHE PATH "Asio include dir" FORCE)

FetchContent_Declare(
    Crow
    GIT_REPOSITORY https://github.com/CrowCpp/Crow.git
    GIT_TAG v1.2.0
)
set(CROW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CROW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CROW_USE_BOOST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(Crow)

FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(json)

FetchContent_Declare(
    cpr
    GIT_REPOSITORY https://github.com/libcpr/cpr.git
    GIT_TAG 1.10.5
)
set(CPR_USE_SYSTEM_CURL ON CACHE BOOL "" FORCE)
set(BUILD_CPR_TESTS OFF CACHE BOOL "" FORCE)
set(CPR_USE_SYSTEM_CURL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(cpr)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.8.0
)
FetchContent_MakeAvailable(Catch2)

find_package(PostgreSQL REQUIRED)
find_package(OpenSSL REQUIRED)

add_library(transit_tracker_core STATIC
    src/core.cpp
    src/app.cpp
)

target_include_directories(transit_tracker_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${ASIO_INCLUDE_DIR}
)

target_compile_definitions(transit_tracker_core
    PUBLIC ASIO_STANDALONE
)

target_link_libraries(transit_tracker_core
    PUBLIC
        Crow::Crow
        nlohmann_json::nlohmann_json
    PRIVATE
        PostgreSQL::PostgreSQL
        OpenSSL::Crypto
)

if(WIN32)
    target_link_libraries(transit_tracker_core PUBLIC ws2_32 mswsock)
endif()

add_executable(kvv_aggregator main.cpp)
add_library(transit_tracker_runtime STATIC src/runtime.cpp)
target_include_directories(transit_tracker_runtime
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${ASIO_INCLUDE_DIR}
)
target_compile_definitions(transit_tracker_runtime PUBLIC ASIO_STANDALONE)
target_link_libraries(transit_tracker_runtime
    PUBLIC
        transit_tracker_core
    PRIVATE
        cpr::cpr
        PostgreSQL::PostgreSQL
)
if(WIN32)
    target_link_libraries(transit_tracker_runtime PUBLIC ws2_32 mswsock)
endif()

target_link_libraries(kvv_aggregator PRIVATE transit_tracker_core transit_tracker_runtime)

if(ENABLE_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    foreach(target transit_tracker_core transit_tracker_runtime kvv_aggregator)
        target_compile_options(${target} PRIVATE -O0 -g --coverage)
        target_link_options(${target} PRIVATE --coverage)
    endforeach()
endif()

if(TRANSIT_TRACKER_BUILD_TESTS)
    enable_testing()
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include(Catch)

    add_executable(transit_tracker_tests
        tests/test_core.cpp
        tests/test_routes.cpp
    )

    target_link_libraries(transit_tracker_tests
        PRIVATE
            transit_tracker_core
            Catch2::Catch2WithMain
    )

    if(ENABLE_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(transit_tracker_tests PRIVATE -O0 -g --coverage)
        target_link_options(transit_tracker_tests PRIVATE --coverage)
    endif()

    catch_discover_tests(transit_tracker_tests)

    if(ENABLE_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        find_program(GCOVR_EXECUTABLE gcovr)
        if(GCOVR_EXECUTABLE)
            add_custom_target(coverage
                COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
                COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/coverage
                COMMAND ${GCOVR_EXECUTABLE}
                    --root ${CMAKE_SOURCE_DIR}
                    --filter ${CMAKE_SOURCE_DIR}/src
                    --filter ${CMAKE_SOURCE_DIR}/include
                    --exclude-directories ${CMAKE_BINARY_DIR}/_deps
                    --html-details ${CMAKE_BINARY_DIR}/coverage/index.html
                    --xml ${CMAKE_BINARY_DIR}/coverage/coverage.xml
                WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
                DEPENDS transit_tracker_tests
            )
        endif()
    endif()
endif()
