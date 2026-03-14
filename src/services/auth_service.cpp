#include "auth_service.h"
#include <openssl/evp.h>
#include <sstream>
#include <iomanip>
#include <mutex>
#include <map>
#include <chrono>
#include <iostream>

// Persistent auth DB connection and throttle state (protected by auth_db_mutex)
static std::mutex auth_db_mutex;
static PGconn* auth_db_conn = nullptr;
static std::map<std::string, std::chrono::steady_clock::time_point> last_used_updates;
static constexpr int LAST_USED_UPDATE_INTERVAL_SECONDS = 300; // 5 minutes

std::string sha256Hex(const std::string& input) {
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

// Constant-time string comparison to prevent timing attacks
bool constantTimeEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    volatile unsigned char result = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        result |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    }
    return result == 0;
}

static PGconn* getAuthDbConnection(const Database& db) {
    // Must be called with auth_db_mutex held
    if (auth_db_conn && PQstatus(auth_db_conn) == CONNECTION_OK) {
        return auth_db_conn;
    }
    // Clean up stale connection
    if (auth_db_conn) {
        PQfinish(auth_db_conn);
        auth_db_conn = nullptr;
    }
    if (!db.hasConfig()) return nullptr;
    auth_db_conn = db.connect();
    if (!auth_db_conn) return nullptr;
    if (PQstatus(auth_db_conn) != CONNECTION_OK) {
        std::cerr << "Auth DB connection failed: " << PQerrorMessage(auth_db_conn) << std::endl;
        PQfinish(auth_db_conn);
        auth_db_conn = nullptr;
        return nullptr;
    }
    return auth_db_conn;
}

bool validateKeyViaDatabase(const std::string& providedKey, const Database& db) {
    if (!db.hasConfig()) return false;

    std::string keyHash = sha256Hex(providedKey);
    if (keyHash.empty()) return false;

    std::lock_guard<std::mutex> lock(auth_db_mutex);

    // Purge stale entries from the throttle map to prevent unbounded growth
    auto now = std::chrono::steady_clock::now();
    for (auto it = last_used_updates.begin(); it != last_used_updates.end(); ) {
        if (std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count() >= LAST_USED_UPDATE_INTERVAL_SECONDS) {
            it = last_used_updates.erase(it);
        } else {
            ++it;
        }
    }

    PGconn* conn = getAuthDbConnection(db);
    if (!conn) return false;

    const char* querySql =
        "SELECT id FROM api_keys "
        "WHERE key_hash = $1 AND revoked = FALSE "
        "AND (expires_at IS NULL OR expires_at > NOW())";
    const char* queryValues[1] = { keyHash.c_str() };

    PGresult* res = PQexecParams(conn, querySql, 1, nullptr, queryValues, nullptr, nullptr, 0);
    if (!res) {
        std::cerr << "Auth query returned null result" << std::endl;
        return false;
    }
    bool valid = (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0);

    if (valid) {
        std::string keyId = PQgetvalue(res, 0, 0);
        PQclear(res);

        // Throttle last_used_at updates: only once per LAST_USED_UPDATE_INTERVAL_SECONDS per key
        auto now = std::chrono::steady_clock::now();
        auto it = last_used_updates.find(keyId);
        bool shouldUpdate = (it == last_used_updates.end()) ||
            (std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count() >= LAST_USED_UPDATE_INTERVAL_SECONDS);

        if (shouldUpdate) {
            const char* updateSql = "UPDATE api_keys SET last_used_at = NOW() WHERE id = $1";
            const char* updateValues[1] = { keyId.c_str() };
            PGresult* updateRes = PQexecParams(conn, updateSql, 1, nullptr, updateValues, nullptr, nullptr, 0);
            if (!updateRes || PQresultStatus(updateRes) != PGRES_COMMAND_OK) {
                std::cerr << "Failed to update last_used_at: " << PQerrorMessage(conn) << std::endl;
            } else {
                last_used_updates[keyId] = now;
            }
            if (updateRes) PQclear(updateRes);
        }
    } else {
        PQclear(res);
    }

    return valid;
}
