#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <nlohmann/json.hpp>

#include "../../main.cpp"

using json = nlohmann::json;

TEST_CASE("isValidStopId validates expected formats") {
    REQUIRE(isValidStopId("7000107"));
    REQUIRE(isValidStopId("de:08212:107"));
    REQUIRE_FALSE(isValidStopId(""));
    REQUIRE_FALSE(isValidStopId("invalid\nvalue"));
    REQUIRE_FALSE(isValidStopId("bad$char"));
}

TEST_CASE("isValidSearchQuery rejects control chars and long values") {
    REQUIRE(isValidSearchQuery("Knielingen"));
    REQUIRE_FALSE(isValidSearchQuery(""));
    REQUIRE_FALSE(isValidSearchQuery(std::string("A\x01B", 3)));
    REQUIRE_FALSE(isValidSearchQuery(std::string(201, 'x')));
}

TEST_CASE("extractCoordinates supports array and object layouts") {
    double lat = 0.0;
    double lon = 0.0;

    const json coordArray = {
        {"coord", json::array({49.01, 8.4})}
    };
    REQUIRE(extractCoordinates(coordArray, lat, lon));
    REQUIRE(lat == Catch::Approx(49.01));
    REQUIRE(lon == Catch::Approx(8.4));

    const json coordObject = {
        {"coord", {
            {"x", "49.02"},
            {"y", "8.41"}
        }}
    };
    REQUIRE(extractCoordinates(coordObject, lat, lon));
    REQUIRE(lat == Catch::Approx(49.02));
    REQUIRE(lon == Catch::Approx(8.41));
}

TEST_CASE("normalizeResponse adds detailed and delay fields when requested") {
    const json providerData = {
        {"departureList", json::array({
            {
                {"servingLine", {
                    {"number", "2"},
                    {"direction", "Knielingen"},
                    {"motType", "4"},
                    {"delay", "3"},
                    {"hints", json::array({{{"hint", "low floor"}}})}
                }},
                {"attrs", json::array({
                    {{"name", "planLowFloorVehicle"}, {"value", "1"}},
                    {{"name", "planWheelchairAccess"}, {"value", "1"}}
                })},
                {"platform", "1"},
                {"countdown", "5"},
                {"realDateTime", "2026-01-01T10:00:00"}
            }
        })}
    };

    const json normalized = normalizeResponse(providerData, true, true);
    REQUIRE(normalized.is_array());
    REQUIRE(normalized.size() == 1);

    const auto& dep = normalized.at(0);
    REQUIRE(dep.at("line") == "2");
    REQUIRE(dep.at("minutes_remaining") == 5);
    REQUIRE(dep.at("is_realtime") == true);
    REQUIRE(dep.at("delay_minutes") == 3);
    REQUIRE(dep.at("low_floor") == true);
    REQUIRE(dep.at("wheelchair_accessible") == true);
}
