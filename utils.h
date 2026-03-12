#ifndef UTILS_H
#define UTILS_H

#include <string>
#include <optional>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <ctime>
#include <regex>
#include <locale>
#include <vector>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

using json = nlohmann::json;

// --- Constants ---
inline constexpr size_t MAX_QUERY_LENGTH = 200;
inline constexpr size_t MAX_STOPID_LENGTH = 100;

// --- Input Validation ---
inline bool isValidStopId(const std::string& stopId) {
    if (stopId.empty() || stopId.size() > MAX_STOPID_LENGTH) return false;
    static const std::regex stopIdPattern("^[a-zA-Z0-9:_. -]+$");
    return std::regex_match(stopId, stopIdPattern);
}

inline bool isValidSearchQuery(const std::string& query) {
    if (query.empty() || query.size() > MAX_QUERY_LENGTH) return false;
    for (unsigned char c : query) {
        if (c < 0x20 || c == 0x7F) return false;
    }
    return true;
}

// --- String Utilities ---
inline std::string toLower(const std::string& str) {
    std::string s = str;
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return s;
}

inline std::string trim(const std::string& input) {
    auto start = std::find_if_not(input.begin(), input.end(),
                                  [](unsigned char c){ return std::isspace(c); });
    auto end = std::find_if_not(input.rbegin(), input.rend(),
                                [](unsigned char c){ return std::isspace(c); }).base();
    if (start >= end) return "";
    return std::string(start, end);
}

// --- Security ---
inline std::string sha256Hex(const std::string& input) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hashLen = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return "";
    bool ok = EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1
           && EVP_DigestUpdate(ctx, input.c_str(), input.size()) == 1
           && EVP_DigestFinal_ex(ctx, hash, &hashLen) == 1;
    EVP_MD_CTX_free(ctx);
    if (!ok || hashLen == 0) return "";
    std::ostringstream ss;
    for (unsigned int i = 0; i < hashLen; i++) {
        ss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(hash[i]);
    }
    return ss.str();
}

inline bool constantTimeEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    volatile unsigned char result = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        result |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    }
    return result == 0;
}

// --- JSON Helpers ---
inline std::optional<std::string> jsonToString(const json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_number_integer()) return std::to_string(value.get<long long>());
    if (value.is_number_float()) {
        std::ostringstream stream;
        stream << std::setprecision(15) << value.get<double>();
        return stream.str();
    }
    return std::nullopt;
}

inline std::optional<double> jsonToDouble(const json& value) {
    if (value.is_number()) return value.get<double>();
    if (value.is_string()) {
        try {
            return std::stod(value.get<std::string>());
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

inline std::optional<std::string> getJsonString(const json& obj, std::initializer_list<const char*> keys) {
    for (const auto* key : keys) {
        if (obj.contains(key)) {
            auto value = jsonToString(obj.at(key));
            if (value && !value->empty()) return value;
        }
    }
    return std::nullopt;
}

inline std::string formatDouble(double value) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(8) << value;
    return stream.str();
}

// --- Date/Time ---
inline std::optional<std::time_t> parseISO8601(const std::string& timestamp) {
    std::tm tm = {};
    std::istringstream ss(timestamp);
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (ss.fail()) return std::nullopt;
    return timegm(&tm);
}

// --- MOT (Mode of Transport) Parsing ---
inline std::optional<std::string> buildMotArray(const json& stop) {
    std::vector<int> motValues;

    auto addValue = [&](const json& value) {
        if (value.is_number_integer()) {
            motValues.push_back(value.get<int>());
        } else if (value.is_string()) {
            try {
                motValues.push_back(std::stoi(value.get<std::string>()));
            } catch (...) {
                return;
            }
        } else if (value.is_object()) {
            auto nested = getJsonString(value, {"motType", "type", "mode"});
            if (nested) {
                try {
                    motValues.push_back(std::stoi(*nested));
                } catch (...) {
                    return;
                }
            }
        }
    };

    if (stop.contains("modes")) {
        const auto& modes = stop.at("modes");
        if (modes.is_array()) {
            for (const auto& item : modes) addValue(item);
        } else {
            addValue(modes);
        }
    } else if (stop.contains("mot")) {
        const auto& mot = stop.at("mot");
        if (mot.is_array()) {
            for (const auto& item : mot) addValue(item);
        } else {
            addValue(mot);
        }
    } else if (stop.contains("productClasses")) {
        const auto& classes = stop.at("productClasses");
        if (classes.is_array()) {
            for (const auto& item : classes) addValue(item);
        } else {
            addValue(classes);
        }
    } else if (stop.contains("motType")) {
        addValue(stop.at("motType"));
    }

    if (motValues.empty()) return std::nullopt;

    std::ostringstream stream;
    stream << "{";
    for (size_t i = 0; i < motValues.size(); ++i) {
        if (i > 0) stream << ",";
        stream << motValues[i];
    }
    stream << "}";
    return stream.str();
}

// --- Coordinate Extraction ---
inline bool extractCoordinates(const json& stop, double& lat, double& lon) {
    auto extractFromObject = [&](const json& coord) -> bool {
        if (coord.contains("x") && coord.contains("y")) {
            auto latValue = jsonToDouble(coord.at("x"));
            auto lonValue = jsonToDouble(coord.at("y"));
            if (latValue && lonValue) {
                lat = *latValue;
                lon = *lonValue;
                return true;
            }
        }
        if (coord.contains("lon") && coord.contains("lat")) {
            auto latValue = jsonToDouble(coord.at("lat"));
            auto lonValue = jsonToDouble(coord.at("lon"));
            if (latValue && lonValue) {
                lat = *latValue;
                lon = *lonValue;
                return true;
            }
        }
        if (coord.contains("longitude") && coord.contains("latitude")) {
            auto latValue = jsonToDouble(coord.at("latitude"));
            auto lonValue = jsonToDouble(coord.at("longitude"));
            if (latValue && lonValue) {
                lat = *latValue;
                lon = *lonValue;
                return true;
            }
        }
        return false;
    };

    if (stop.contains("coord")) {
        const auto& coord = stop.at("coord");
        if (coord.is_object() && extractFromObject(coord)) return true;
        if (coord.is_array() && coord.size() >= 2) {
            // Stopfinder arrays use [latitude, longitude] ordering.
            auto latValue = jsonToDouble(coord.at(0));
            auto lonValue = jsonToDouble(coord.at(1));
            if (latValue && lonValue) {
                lat = *latValue;
                lon = *lonValue;
                return true;
            }
        }
    }

    if (extractFromObject(stop)) return true;

    if (stop.contains("latitude") && stop.contains("longitude")) {
        auto latValue = jsonToDouble(stop.at("latitude"));
        auto lonValue = jsonToDouble(stop.at("longitude"));
        if (latValue && lonValue) {
            lat = *latValue;
            lon = *lonValue;
            return true;
        }
    }

    return false;
}

#endif // UTILS_H
