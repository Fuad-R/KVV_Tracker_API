// Unit tests for utility functions in utils.h
// Framework: Catch2 v3
// These tests run without network or database access.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "utils.h"

// ============================================================================
// isValidStopId
// ============================================================================

TEST_CASE("isValidStopId accepts valid stop IDs", "[validation]") {
    CHECK(isValidStopId("7000107"));
    CHECK(isValidStopId("de:08212:107"));
    CHECK(isValidStopId("Karlsruhe Hbf"));
    CHECK(isValidStopId("stop_123"));
    CHECK(isValidStopId("stop-456"));
    CHECK(isValidStopId("A"));
}

TEST_CASE("isValidStopId rejects invalid stop IDs", "[validation]") {
    CHECK_FALSE(isValidStopId(""));
    CHECK_FALSE(isValidStopId(std::string(101, 'a'))); // exceeds MAX_STOPID_LENGTH
    CHECK_FALSE(isValidStopId("stop<script>"));        // disallowed characters
    CHECK_FALSE(isValidStopId("stop;drop"));
    CHECK_FALSE(isValidStopId("stop\ttab"));
    CHECK_FALSE(isValidStopId("stop\nnewline"));
}

TEST_CASE("isValidStopId boundary length", "[validation]") {
    CHECK(isValidStopId(std::string(100, 'x')));        // exactly 100
    CHECK_FALSE(isValidStopId(std::string(101, 'x')));  // 101
}

// ============================================================================
// isValidSearchQuery
// ============================================================================

TEST_CASE("isValidSearchQuery accepts valid queries", "[validation]") {
    CHECK(isValidSearchQuery("Knielingen"));
    CHECK(isValidSearchQuery("Karlsruhe Hbf"));
    CHECK(isValidSearchQuery("123"));
    CHECK(isValidSearchQuery("stop with spaces and CAPS"));
    CHECK(isValidSearchQuery("Ubahn/S-Bahn"));
}

TEST_CASE("isValidSearchQuery rejects invalid queries", "[validation]") {
    CHECK_FALSE(isValidSearchQuery(""));
    CHECK_FALSE(isValidSearchQuery(std::string(201, 'a'))); // exceeds MAX_QUERY_LENGTH
    CHECK_FALSE(isValidSearchQuery("query\twith\ttab"));     // control characters
    CHECK_FALSE(isValidSearchQuery("query\nwith\nnewline"));
    CHECK_FALSE(isValidSearchQuery(std::string(1, '\x01'))); // control char
    CHECK_FALSE(isValidSearchQuery(std::string(1, '\x7F'))); // DEL char
}

TEST_CASE("isValidSearchQuery boundary length", "[validation]") {
    CHECK(isValidSearchQuery(std::string(200, 'x')));        // exactly 200
    CHECK_FALSE(isValidSearchQuery(std::string(201, 'x')));  // 201
}

// ============================================================================
// toLower
// ============================================================================

TEST_CASE("toLower converts to lowercase", "[string]") {
    CHECK(toLower("HELLO") == "hello");
    CHECK(toLower("Hello World") == "hello world");
    CHECK(toLower("already lower") == "already lower");
    CHECK(toLower("") == "");
    CHECK(toLower("123ABC") == "123abc");
}

// ============================================================================
// trim
// ============================================================================

TEST_CASE("trim removes whitespace", "[string]") {
    CHECK(trim("  hello  ") == "hello");
    CHECK(trim("\thello\t") == "hello");
    CHECK(trim("\n hello \n") == "hello");
    CHECK(trim("hello") == "hello");
    CHECK(trim("") == "");
    CHECK(trim("   ") == "");
    CHECK(trim("  hello world  ") == "hello world");
}

// ============================================================================
// sha256Hex
// ============================================================================

TEST_CASE("sha256Hex produces correct hash", "[security]") {
    // SHA-256 of empty string
    CHECK(sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    // SHA-256 of "hello"
    CHECK(sha256Hex("hello") == "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
    // SHA-256 of "test-api-key"
    std::string hash = sha256Hex("test-api-key");
    CHECK(hash.size() == 64);  // 256 bits = 64 hex chars
    // All chars should be valid hex
    for (char c : hash) {
        CHECK(((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')));
    }
}

// ============================================================================
// constantTimeEquals
// ============================================================================

TEST_CASE("constantTimeEquals compares strings correctly", "[security]") {
    CHECK(constantTimeEquals("hello", "hello"));
    CHECK(constantTimeEquals("", ""));
    CHECK_FALSE(constantTimeEquals("hello", "world"));
    CHECK_FALSE(constantTimeEquals("hello", "hell"));   // different lengths
    CHECK_FALSE(constantTimeEquals("hello", "hellp"));   // one char different
}

// ============================================================================
// formatDouble
// ============================================================================

TEST_CASE("formatDouble formats with 8 decimal places", "[formatting]") {
    std::string result = formatDouble(48.12345678);
    CHECK(result == "48.12345678");

    result = formatDouble(0.0);
    CHECK(result == "0.00000000");

    result = formatDouble(-8.12345678);
    CHECK(result == "-8.12345678");

    // Ensure dot separator (not comma) regardless of locale
    result = formatDouble(1.5);
    CHECK(result.find('.') != std::string::npos);
    CHECK(result.find(',') == std::string::npos);
}

// ============================================================================
// parseISO8601
// ============================================================================

TEST_CASE("parseISO8601 parses valid timestamps", "[datetime]") {
    auto result = parseISO8601("2025-01-15T12:30:00");
    REQUIRE(result.has_value());
    CHECK(*result > 0);

    // 1970-01-01T00:00:00 should be epoch 0
    auto epoch = parseISO8601("1970-01-01T00:00:00");
    REQUIRE(epoch.has_value());
    CHECK(*epoch == 0);
}

TEST_CASE("parseISO8601 rejects invalid timestamps", "[datetime]") {
    CHECK_FALSE(parseISO8601("not-a-date").has_value());
    CHECK_FALSE(parseISO8601("").has_value());
}

// ============================================================================
// jsonToString
// ============================================================================

TEST_CASE("jsonToString converts JSON values to strings", "[json]") {
    CHECK(jsonToString(json("hello")) == "hello");
    CHECK(jsonToString(json(42)) == "42");
    CHECK(jsonToString(json(3.14)).has_value());

    // Null and objects return nullopt
    CHECK_FALSE(jsonToString(json(nullptr)).has_value());
    CHECK_FALSE(jsonToString(json::object()).has_value());
    CHECK_FALSE(jsonToString(json::array()).has_value());
}

// ============================================================================
// jsonToDouble
// ============================================================================

TEST_CASE("jsonToDouble converts JSON values to double", "[json]") {
    auto result = jsonToDouble(json(3.14));
    REQUIRE(result.has_value());
    CHECK_THAT(*result, Catch::Matchers::WithinAbs(3.14, 0.001));

    result = jsonToDouble(json(42));
    REQUIRE(result.has_value());
    CHECK_THAT(*result, Catch::Matchers::WithinAbs(42.0, 0.001));

    result = jsonToDouble(json("8.123"));
    REQUIRE(result.has_value());
    CHECK_THAT(*result, Catch::Matchers::WithinAbs(8.123, 0.001));

    // Invalid string
    CHECK_FALSE(jsonToDouble(json("not_a_number")).has_value());
    CHECK_FALSE(jsonToDouble(json(nullptr)).has_value());
}

// ============================================================================
// getJsonString
// ============================================================================

TEST_CASE("getJsonString searches multiple keys", "[json]") {
    json obj = {{"name", "Test"}, {"id", 123}};

    auto result = getJsonString(obj, {"name", "title"});
    REQUIRE(result.has_value());
    CHECK(*result == "Test");

    // Falls back to second key
    result = getJsonString(obj, {"title", "name"});
    REQUIRE(result.has_value());
    CHECK(*result == "Test");

    // No matching key
    result = getJsonString(obj, {"missing", "also_missing"});
    CHECK_FALSE(result.has_value());
}

// ============================================================================
// buildMotArray
// ============================================================================

TEST_CASE("buildMotArray extracts MOT values", "[mot]") {
    // modes array with integers
    json stop = {{"modes", json::array({1, 2, 3})}};
    auto result = buildMotArray(stop);
    REQUIRE(result.has_value());
    CHECK(*result == "{1,2,3}");

    // mot array with strings
    stop = {{"mot", json::array({"4", "5"})}};
    result = buildMotArray(stop);
    REQUIRE(result.has_value());
    CHECK(*result == "{4,5}");

    // Single motType value
    stop = {{"motType", 7}};
    result = buildMotArray(stop);
    REQUIRE(result.has_value());
    CHECK(*result == "{7}");

    // No MOT data
    stop = {{"name", "test"}};
    result = buildMotArray(stop);
    CHECK_FALSE(result.has_value());
}

// ============================================================================
// extractCoordinates
// ============================================================================

TEST_CASE("extractCoordinates from coord object with x/y", "[coordinates]") {
    // EFA Stopfinder format: x=latitude, y=longitude
    json stop = {{"coord", {{"x", 49.0069}, {"y", 8.4037}}}};
    double lat = 0, lon = 0;
    REQUIRE(extractCoordinates(stop, lat, lon));
    CHECK_THAT(lat, Catch::Matchers::WithinAbs(49.0069, 0.0001));
    CHECK_THAT(lon, Catch::Matchers::WithinAbs(8.4037, 0.0001));
}

TEST_CASE("extractCoordinates from coord object with lat/lon", "[coordinates]") {
    json stop = {{"coord", {{"lat", 49.0069}, {"lon", 8.4037}}}};
    double lat = 0, lon = 0;
    REQUIRE(extractCoordinates(stop, lat, lon));
    CHECK_THAT(lat, Catch::Matchers::WithinAbs(49.0069, 0.0001));
    CHECK_THAT(lon, Catch::Matchers::WithinAbs(8.4037, 0.0001));
}

TEST_CASE("extractCoordinates from coord array", "[coordinates]") {
    // Stopfinder coordinate array format: [latitude, longitude]
    json stop = {{"coord", json::array({49.0069, 8.4037})}};
    double lat = 0, lon = 0;
    REQUIRE(extractCoordinates(stop, lat, lon));
    CHECK_THAT(lat, Catch::Matchers::WithinAbs(49.0069, 0.0001));
    CHECK_THAT(lon, Catch::Matchers::WithinAbs(8.4037, 0.0001));
}

TEST_CASE("extractCoordinates from top-level latitude/longitude", "[coordinates]") {
    json stop = {{"latitude", 49.0069}, {"longitude", 8.4037}};
    double lat = 0, lon = 0;
    REQUIRE(extractCoordinates(stop, lat, lon));
    CHECK_THAT(lat, Catch::Matchers::WithinAbs(49.0069, 0.0001));
    CHECK_THAT(lon, Catch::Matchers::WithinAbs(8.4037, 0.0001));
}

TEST_CASE("extractCoordinates returns false for missing data", "[coordinates]") {
    json stop = {{"name", "No coords"}};
    double lat = 0, lon = 0;
    CHECK_FALSE(extractCoordinates(stop, lat, lon));
}
