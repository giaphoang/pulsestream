#pragma once
#include "pulse/service.hpp"
#include <condition_variable>
#include <deque>
#include <functional>
namespace pulse {
Request parse_http_head(const std::string& head, size_t& content_length);
class HttpServer {
    Config config_;
    std::function<Response(const Request&)> handler_;
public:
    HttpServer(Config c,std::function<Response(const Request&)> handler):config_(std::move(c)),handler_(std::move(handler)){}
    void run(const std::function<bool()>& stopping);
};
Response http_call(const std::string& host,int port,const Request& request);
}
