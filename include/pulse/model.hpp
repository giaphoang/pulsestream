#pragma once
#include <nlohmann/json.hpp>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
namespace pulse {
using Json = nlohmann::json;
struct Error : std::runtime_error {
    int status;
    Error(int s, const std::string& message) : std::runtime_error(message), status(s) {}
};
int64_t now_ms();
bool valid_id(std::string_view value);
std::string require_id(const Json&, const char* key);
int64_t integer(const Json&, const char* key, int64_t low, int64_t high);
std::string digest(std::string_view);
bool secret_equal(std::string_view, std::string_view);
Json validate_event(const Json&, int64_t now);
std::string transmission_state(int received, int total, int64_t due, int64_t last_received, int64_t now);
struct Config {
    std::string database, wal_directory, operator_token, clinics;
    std::string bind = "127.0.0.1";
    int port = 8080, http_threads = 8, workers = 4;
    size_t max_connections = 256;
    uint32_t queue_items = 100000;
    uint32_t lease_ms = 30000;
    std::string crash_at;
    static Config environment();
};
void crash_point(const Config&, const char* stage);
}
