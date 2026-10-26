#include "pulse/http.hpp"
#include <csignal>
#include <iostream>
namespace { volatile std::sig_atomic_t stopping=0; void stop(int) { stopping=1; } }
int main() {
    try {
        std::signal(SIGINT,stop); std::signal(SIGTERM,stop); std::signal(SIGPIPE,SIG_IGN);
        auto config=pulse::Config::environment(); pulse::Service service(config); service.initialize(); service.start();
        pulse::HttpServer server(config,[&](const auto& r){return service.handle(r);});
        std::cout<<"PulseStream synthetic telemetry listening on "<<config.bind<<":"<<config.port<<std::endl;
        server.run([] {return stopping!=0;}); service.stop(); return 0;
    } catch(const std::exception& e) { std::cerr<<"startup failed: "<<e.what()<<'\n'; return 1; }
}
