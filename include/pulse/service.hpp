#pragma once
#include "pulse/database.hpp"
#include "pulse/queue.hpp"
#include <atomic>
#include <map>
#include <thread>
#include <vector>
namespace pulse {
struct Request {
    std::string method, path, body;
    std::map<std::string,std::string> headers;
};
struct Response {
    int status=200;
    std::string body;
    std::string content_type="application/json";
};
class Service {
    Config config_;
    Queue queue_;
    std::atomic<bool> stopping_{false};
    std::vector<std::thread> workers_;
    std::mutex admission_;
    std::atomic<uint64_t> accepted_{0}, duplicates_{0}, rejected_{0}, processed_{0}, retries_{0};
    std::string bearer(const Request&) const;
    std::string clinic(Database&,const Request&) const;
    void worker();
    Response route(const Request&);
public:
    explicit Service(Config);
    ~Service();
    void initialize();
    void start();
    void stop();
    Response handle(const Request&);
};
}
