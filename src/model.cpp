#include "pulse/model.hpp"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <array>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <set>
#include <unistd.h>
namespace pulse {
int64_t now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
bool valid_id(std::string_view s) {
    if(s.empty() || s.size() > 64) return false;
    for(unsigned char c : s) if(!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    return true;
}
std::string require_id(const Json& j, const char* key) {
    if(!j.contains(key) || !j[key].is_string() || !valid_id(j[key].get<std::string>())) throw Error(422, std::string("invalid ") + key);
    return j[key].get<std::string>();
}
int64_t integer(const Json& j, const char* key, int64_t low, int64_t high) {
    if(!j.contains(key) || !j[key].is_number_integer()) throw Error(422, std::string("invalid ") + key);
    if(j[key].is_number_unsigned() && j[key].get<uint64_t>() > static_cast<uint64_t>(INT64_MAX)) throw Error(422, std::string("out of range ") + key);
    auto v = j[key].get<int64_t>();
    if(v < low || v > high) throw Error(422, std::string("out of range ") + key);
    return v;
}
std::string digest(std::string_view value) {
    unsigned char bytes[EVP_MAX_MD_SIZE]; unsigned int size = 0;
    if(EVP_Digest(value.data(), value.size(), bytes, &size, EVP_sha256(), nullptr) != 1) throw std::runtime_error("SHA256 failure");
    const char* hex = "0123456789abcdef"; std::string out;
    for(unsigned int i = 0; i < size; ++i) { out += hex[bytes[i] >> 4]; out += hex[bytes[i] & 15]; }
    return out;
}
bool secret_equal(std::string_view a, std::string_view b) { return a.size() == b.size() && CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0; }
Json validate_event(const Json& j, int64_t now) {
    if(!j.is_object()) throw Error(422, "event must be an object");
    static const std::set<std::string> allowed = {"device_id","event_id","timestamp_ms","sequence","schema_version","type","battery_pct","diagnostic_flag","transmission_id","part_index","parts_total","status"};
    for(const auto& [key, value] : j.items()) { (void)value; if(!allowed.contains(key)) throw Error(422, "unknown event field: " + key); }
    require_id(j,"device_id"); require_id(j,"event_id");
    integer(j,"timestamp_ms",1,now + 300000); integer(j,"sequence",1,INT64_MAX); integer(j,"schema_version",1,1);
    auto type = require_id(j,"type");
    if(type == "heartbeat") integer(j,"battery_pct",0,100);
    else if(type == "scheduled_transmission") {
        require_id(j,"transmission_id"); auto total = integer(j,"parts_total",1,1024); integer(j,"part_index",0,total-1);
    } else if(type == "diagnostic_event") require_id(j,"diagnostic_flag");
    else if(type == "device_status") {
        auto status = require_id(j,"status");
        if(status != "low_battery" && status != "communication_failure" && status != "reconnect") throw Error(422,"invalid status");
    } else throw Error(422,"unsupported event type");
    const std::set<std::string> specific = {"battery_pct","diagnostic_flag","transmission_id","part_index","parts_total","status"};
    for(const auto& key : specific) if(j.contains(key)) {
        bool ok = (type == "heartbeat" && key == "battery_pct") || (type == "diagnostic_event" && key == "diagnostic_flag") || (type == "device_status" && key == "status") || (type == "scheduled_transmission" && (key == "transmission_id" || key == "part_index" || key == "parts_total"));
        if(!ok) throw Error(422,"field does not belong to event type");
    }
    return j;
}
std::string transmission_state(int received, int total, int64_t due, int64_t last, int64_t now) {
    if(received >= total) return last > due ? "delayed" : "received";
    if(now > due) return "overdue";
    return received > 0 ? "incomplete" : "expected";
}
Config Config::environment() {
    auto env = [](const char* key, const char* fallback = "") { const char* v = std::getenv(key); return std::string(v ? v : fallback); };
    Config c; c.database=env("PULSE_DATABASE_URL"); c.wal_directory=env("PULSE_WAL_DIR","./data/wal"); c.operator_token=env("PULSE_OPERATOR_TOKEN"); c.clinics=env("PULSE_CLINICS","[]"); c.bind=env("PULSE_BIND","127.0.0.1");
    auto num = [&](const char* key, int fallback, int low, int high) { auto s=env(key,std::to_string(fallback).c_str()); size_t used=0; int v=std::stoi(s,&used); if(used!=s.size() || v<low || v>high) throw std::runtime_error(std::string("invalid config ")+key); return v; };
    c.port=num("PULSE_PORT",8080,1,65535); c.http_threads=num("PULSE_HTTP_THREADS",8,1,128); c.workers=num("PULSE_WORKERS",4,1,32); c.queue_items=num("PULSE_QUEUE_ITEMS",100000,1,1000000);
    c.crash_at=env("PULSE_CRASH_AT");
    if(!c.crash_at.empty() && env("PULSE_ENABLE_TEST_HOOKS") != "1") throw std::runtime_error("crash hooks require PULSE_ENABLE_TEST_HOOKS=1");
    if(!env("PULSE_TEST_LEASE_MS").empty()) {
        if(env("PULSE_ENABLE_TEST_HOOKS")!="1") throw std::runtime_error("test leases require PULSE_ENABLE_TEST_HOOKS=1");
        c.lease_ms=num("PULSE_TEST_LEASE_MS",30000,10,30000);
    }
    if(c.database.empty() || c.operator_token.size()<16) throw std::runtime_error("database and operator token (16+ characters) required");
    return c;
}
void crash_point(const Config& c, const char* stage) { if(c.crash_at == stage) _exit(86); }
}
