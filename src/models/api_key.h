#pragma once

#include <string>
#include <optional>

struct ApiKey {
    std::string id;
    std::string key_hash;
    bool revoked = false;
    std::optional<std::string> expires_at;
    std::optional<std::string> last_used_at;
};
