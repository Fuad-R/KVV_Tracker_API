#include <chrono>
#include <filesystem>
#include <fstream>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "transit_tracker/core.hpp"

using transit_tracker::CacheEntry;
using transit_tracker::DbConfig;
using transit_tracker::json;

TEST_CASE("validation helpers accept and reject expected values") {
    using namespace transit_tracker;

    CHECK(isValidStopId("de:08212:107"));
    CHECK(isValidStopId("7000107"));
    CHECK_FALSE(isValidStopId(""));
    CHECK_FALSE(isValidStopId("bad/stop"));
    CHECK_FALSE(isValidStopId(std::string(kMaxStopIdLength + 1, 'a')));

    CHECK(isValidSearchQuery("Marktplatz"));
    CHECK_FALSE(isValidSearchQuery(""));
    CHECK_FALSE(isValidSearchQuery("bad\nquery"));
    CHECK_FALSE(isValidSearchQuery(std::string(kMaxQueryLength + 1, 'q')));
}

TEST_CASE("string and json conversion helpers normalize inputs") {
    using namespace transit_tracker;

    CHECK(trim("  Karlsruhe  ") == "Karlsruhe");
    CHECK(trim("   ") == "");
    CHECK(toLower("AbC123") == "abc123");

    CHECK(jsonToString(json("text")).value() == "text");
    CHECK(jsonToString(json(42)).value() == "42");
    CHECK(jsonToString(json(4.25)).value() == "4.25");
    CHECK_FALSE(jsonToString(json::object()));

    CHECK(jsonToDouble(json(12.5)).value() == Catch::Approx(12.5));
    CHECK(jsonToDouble(json("13.75")).value() == Catch::Approx(13.75));
    CHECK_FALSE(jsonToDouble(json("nope")));
}

TEST_CASE("database config parsing handles comments defaults and missing fields") {
    using namespace transit_tracker;
    namespace fs = std::filesystem;

    auto temp_dir = fs::temp_directory_path() / "transit_tracker_tests";
    fs::create_directories(temp_dir);

    auto valid_path = temp_dir / "db_valid.txt";
    {
        std::ofstream out(valid_path);
        out << "# comment\n";
        out << " host = localhost \n";
        out << "port=5432\n";
        out << "dbname = transit\n";
        out << "user = app\n";
        out << "password = secret\n";
    }

    auto parsed = loadDbConfig(valid_path.string());
    REQUIRE(parsed);
    CHECK(parsed->host == "localhost");
    CHECK(parsed->port == "5432");
    CHECK(parsed->dbname == "transit");
    CHECK(parsed->sslmode == "require");

    auto missing_path = temp_dir / "db_missing.txt";
    {
        std::ofstream out(missing_path);
        out << "host=localhost\n";
        out << "port=5432\n";
    }
    CHECK_FALSE(loadDbConfig(missing_path.string()));
    CHECK_FALSE(loadDbConfig((temp_dir / "not_found.txt").string()));

    fs::remove_all(temp_dir);
}

TEST_CASE("request parameter builders keep provider contracts stable") {
    using namespace transit_tracker;

    auto search = buildSearchStopRequestParameters("Hauptbahnhof");
    CHECK(search.size() == 5);
    CHECK(search[2] == std::make_pair(std::string("name_sf"), std::string("Hauptbahnhof")));

    auto departures = buildDepartureRequestParameters("7000107");
    CHECK(departures[4] == std::make_pair(std::string("name_dm"), std::string("7000107")));

    auto notifications = buildNotificationRequestParameters("7000107");
    CHECK(notifications.back() == std::make_pair(std::string("itdLPxx_selStop"), std::string("7000107")));
}

TEST_CASE("stop parsing extracts modes coordinates and supported result shapes") {
    using namespace transit_tracker;

    json stop = {
        {"id", "de:08212:1"},
        {"name", "Karlsruhe, Marktplatz"},
        {"coord", {49.00937, 8.40390}},
        {"parent", {{"name", "Karlsruhe"}}},
        {"properties", {{"stopId", "7000001"}}},
        {"modes", json::array({3, "4", {{"motType", "5"}}, "bad"})}
    };

    CHECK(buildMotArray(stop).value() == "{3,4,5}");

    double lat = 0.0;
    double lon = 0.0;
    REQUIRE(extractCoordinates(stop, lat, lon));
    CHECK(lat == Catch::Approx(49.00937));
    CHECK(lon == Catch::Approx(8.40390));

    auto record = parseStopRecord(stop);
    REQUIRE(record);
    CHECK(record->stop_id == "de:08212:1");
    CHECK(record->local_id.value() == "7000001");
    CHECK(record->city == "Karlsruhe");

    json alt_coord = {
        {"id", "2"},
        {"name", "Alt"},
        {"longitude", 8.5},
        {"latitude", 49.1}
    };
    REQUIRE(extractCoordinates(alt_coord, lat, lon));
    CHECK(lat == Catch::Approx(49.1));
    CHECK(lon == Catch::Approx(8.5));

    json wrapped = {
        {"stopFinder", {
            {"points", json::array({stop})}
        }}
    };
    auto records = extractStopRecords(wrapped);
    REQUIRE(records.size() == 1);
    CHECK(records.front().stop_name == "Karlsruhe, Marktplatz");

    json fallback = {{"points", json::array({alt_coord})}};
    CHECK(extractStopRecords(fallback).size() == 1);

    json nested_points = {
        {"stopFinder", {
            {"points", {{"point", json::array({stop})}}}
        }}
    };
    CHECK(extractStopRecords(nested_points).size() == 1);

    json invalid = {{"name", "Missing id"}};
    CHECK_FALSE(parseStopRecord(invalid));
}

TEST_CASE("cache helpers separate variants and evict expired entries") {
    using namespace transit_tracker;

    CHECK(buildCacheKey("7000107", false, false) == "7000107");
    CHECK(buildCacheKey("7000107", true, false) == "7000107_detailed");
    CHECK(buildCacheKey("7000107", true, true) == "7000107_detailed_delay");

    CHECK(matchesTrackFilter("1", "1"));
    CHECK(matchesTrackFilter("1a", "1"));
    CHECK(matchesTrackFilter("Gleis 1", "1"));
    CHECK_FALSE(matchesTrackFilter("10", "1"));

    json departures = json::array({
        {{"platform", "1"}, {"line", "2"}},
        {{"platform", "10"}, {"line", "4"}},
        {{"platform", "Gleis 1"}, {"line", "S5"}}
    });
    auto filtered = filterDeparturesByTrack(departures, "1");
    REQUIRE(filtered.size() == 2);

    auto now = std::chrono::steady_clock::now();
    std::map<std::string, CacheEntry> cache = {
        {"fresh", {json::array(), now - std::chrono::seconds(5)}},
        {"stale", {json::array(), now - std::chrono::seconds(kCacheTtlSeconds + 1)}}
    };
    evictExpiredCacheEntries(cache, now);
    CHECK(cache.size() == 1);
    CHECK(cache.count("fresh") == 1);
}

TEST_CASE("normalizeResponse covers standard detailed and delay fields") {
    using namespace transit_tracker;

    json provider = {
        {"departureList", json::array({
            {
                {"servingLine", {
                    {"number", "S5"},
                    {"direction", "Pforzheim"},
                    {"motType", "1"},
                    {"delay", "3"},
                    {"trainType", "TramTrain"},
                    {"trainLength", "long"},
                    {"hints", json::array({
                        {{"hint", "low floor"}},
                        {{"content", "wheelchair access"}}
                    })}
                }},
                {"attrs", json::array({
                    {{"name", "planLowFloorVehicle"}, {"value", "0"}},
                    {{"name", "planWheelchairAccess"}, {"value", "1"}}
                })},
                {"platformName", "3"},
                {"countdown", "5"},
                {"realDateTime", {{"year", 2026}}},
                {"hints", json::array({{{"hint", "Runs today"}}})}
            },
            {
                {"platform", "1"},
                {"countdown", "bad"}
            }
        })}
    };

    auto basic = normalizeResponse(provider, false, false);
    REQUIRE(basic.size() == 2);
    CHECK(basic[0]["line"] == "S5");
    CHECK(basic[0]["direction"] == "Pforzheim");
    CHECK(basic[0]["mot"] == 1);
    CHECK(basic[0]["platform"] == "3");
    CHECK(basic[0]["minutes_remaining"] == 5);
    CHECK(basic[0]["is_realtime"] == true);
    CHECK(basic[1]["platform"] == "1");
    CHECK(basic[1]["minutes_remaining"] == 0);

    auto detailed = normalizeResponse(provider, true, true);
    CHECK(detailed[0]["delay_minutes"] == 3);
    CHECK(detailed[0]["low_floor"] == false);
    CHECK(detailed[0]["wheelchair_accessible"] == true);
    CHECK(detailed[0]["train_type"] == "TramTrain");
    CHECK(detailed[0]["train_length"] == "long");
    REQUIRE(detailed[0].contains("hints"));
    CHECK(detailed[0]["hints"].size() == 1);

    json with_composition = {
        {"departureList", json::array({
            {
                {"servingLine", {
                    {"number", "4"},
                    {"direction", "Oberreut"},
                    {"trainComposition", "double"}
                }}
            }
        })}
    };
    auto composed = normalizeResponse(with_composition, true, false);
    CHECK(composed[0]["train_composition"] == "double");
}

TEST_CASE("time and notification helpers validate ranges and dedupe alerts") {
    using namespace transit_tracker;

    auto parsed = parseISO8601("2026-04-04T12:30:00");
    REQUIRE(parsed);
    CHECK_FALSE(parseISO8601("2026/04/04 12:30:00"));

    json validity = json::array({
        {{"from", "2026-04-04T12:00:00"}, {"to", "2026-04-04T13:00:00"}}
    });
    CHECK(isCurrentlyValid(validity, *parsed));
    CHECK_FALSE(isCurrentlyValid(validity, *parsed + 7200));

    json info = {
        {"id", "a1"},
        {"priority", "high"},
        {"affected", {
            {"stops", json::array({
                {{"properties", {{"stopId", "7000107"}}}}
            })}
        }},
        {"properties", {{"providerCode", "KVV"}}},
        {"infoLinks", json::array({
            {{"urlText", "Alert"}, {"content", ""}, {"subtitle", "Subtitle"}}
        })}
    };
    CHECK(isStopAffected(info, "7000107"));
    CHECK_FALSE(isStopAffected(info, "other"));

    json second_provider = {
        {"infos", {
            {"current", json::array({info})}
        }}
    };
    json first_provider = second_provider;
    auto notifications = extractNotificationsFromResponses("7000107", {first_provider, second_provider});
    REQUIRE(notifications.size() == 1);
    CHECK(notifications[0]["id"] == "a1");
    CHECK(notifications[0]["providerCode"] == "KVV");
    CHECK(notifications[0]["priority"] == "high");
}

TEST_CASE("auth helpers hash and compare values consistently") {
    using namespace transit_tracker;

    CHECK(sha256Hex("secret") == "2bb80d537b1da3e38bd30361aa855686bde0eacd7162fef6a25fe97bf527a25b");
    CHECK(constantTimeEquals("abc", "abc"));
    CHECK_FALSE(constantTimeEquals("abc", "abd"));
    CHECK_FALSE(constantTimeEquals("abc", "ab"));
}
