#include "pulse/http.hpp"
#include <algorithm>
#include <atomic>
#include <fstream>
#include <iostream>
using namespace pulse;
int main(int argc,char** argv) {
    try {
        auto env=[](const char* key) { const char* v=std::getenv(key); if(!v) throw std::runtime_error(std::string("missing ")+key); return std::string(v); };
        int devices=10,events=100,concurrency=4,port=8080; std::string host="127.0.0.1",output="benchmark.json",run="run"+std::to_string(now_ms()),scenario="heartbeat";
        for(int i=1;i<argc;i+=2) { if(i+1>=argc) throw std::runtime_error("option value required"); std::string k=argv[i],v=argv[i+1]; if(k=="--devices") devices=std::stoi(v); else if(k=="--events") events=std::stoi(v); else if(k=="--concurrency") concurrency=std::stoi(v); else if(k=="--port") port=std::stoi(v); else if(k=="--host") host=v; else if(k=="--output") output=v; else if(k=="--run-id") run=v; else if(k=="--scenario") scenario=v; else throw std::runtime_error("unknown option "+k); }
        if(devices<1 || devices>100000 || events<1 || events>100000 || concurrency<1 || concurrency>128 || port<1 || port>65535 || !valid_id(run) || run.size()>40) throw std::runtime_error("invalid workload");
        if(scenario!="heartbeat" && scenario!="mixed") throw std::runtime_error("scenario must be heartbeat or mixed");
        auto op=env("PULSE_OPERATOR_TOKEN"),clinic=env("PULSE_SIM_CLINIC"),token=env("PULSE_SIM_DEVICE_TOKEN");
        std::ofstream manifest(output+".accepted.jsonl");
        if(!manifest) throw std::runtime_error("cannot open acceptance manifest");
        auto call=[&](const std::string& path,const Json& j,const std::string& key) { return http_call(host,port,{"POST",path,j.dump(),{{"authorization","Bearer "+key}}}); };
        for(int i=0;i<devices;++i) {
            Json registration={{"device_id",run+"-d"+std::to_string(i)},{"clinic_id",clinic},{"token",token},{"overdue_after_ms",60000}};
            if(scenario=="mixed") registration["expected_transmissions"]=Json::array({{{"transmission_id","summary"},{"due_ms",now_ms()+60000},{"parts_total",2}}});
            auto response=call("/v1/devices",registration,op);
            if(response.status!=201) throw std::runtime_error("registration failed: "+response.body);
        }
        std::atomic<int> next{0}; std::atomic<uint64_t> failures{0},accepted{0},retry_count{0},bytes{0}; std::mutex mutex; std::vector<double> latencies; std::vector<std::thread> threads;
        Json response_counts=Json::object(), error_samples=Json::array();
        auto start=std::chrono::steady_clock::now();
        for(int t=0;t<concurrency;++t) threads.emplace_back([&] {
            for(int d=next++;d<devices;d=next++) for(int i=0;i<events;++i) {
                Json event={{"device_id",run+"-d"+std::to_string(d)},{"event_id","e"+std::to_string(i)},{"type","heartbeat"},{"schema_version",1},{"sequence",i+1},{"timestamp_ms",now_ms()},{"battery_pct",90}};
                if(scenario=="mixed" && i>=1 && i<=4) {
                    event.erase("battery_pct");
                    if(i==1) {event["type"]="diagnostic_event";event["diagnostic_flag"]="synthetic_review_flag";}
                    else if(i==4) {event["type"]="device_status";event["status"]="reconnect";}
                    else {event["type"]="scheduled_transmission";event["transmission_id"]="summary";event["part_index"]=i-2;event["parts_total"]=2;}
                }
                bytes+=event.dump().size(); auto began=std::chrono::steady_clock::now(); bool ok=false;
                for(int attempt=0;attempt<5;++attempt) {
                    try {
                        auto r=call("/v1/events",event,token);
                        {std::lock_guard lock(mutex);auto key=std::to_string(r.status);response_counts[key]=response_counts.value(key,uint64_t{0})+1;if(r.status>=400 && error_samples.size()<5) error_samples.push_back({{"status",r.status},{"body",r.body}});}
                        if(r.status==200 || r.status==202) {ok=true;break;} if(r.status<500) break;
                    } catch(const std::exception& e) {std::lock_guard lock(mutex);response_counts["transport_error"]=response_counts.value("transport_error",uint64_t{0})+1;if(error_samples.size()<5)error_samples.push_back({{"transport_error",e.what()}});}
                    ++retry_count; std::this_thread::sleep_for(std::chrono::milliseconds(50*(attempt+1)));
                }
                if(ok) { ++accepted; auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-began).count(); std::lock_guard lock(mutex); latencies.push_back(ms); manifest<<Json({{"device_id",event.at("device_id")},{"event_id",event.at("event_id")}}).dump()<<'\n'; } else ++failures;
            }
        });
        for(auto& thread:threads) thread.join(); auto seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count(); std::sort(latencies.begin(),latencies.end());
        auto percentile=[&](double p) { return latencies.empty() ? 0.0 : latencies[static_cast<size_t>((latencies.size()-1)*p)]; };
        Json report={{"run_id",run},{"devices",devices},{"events_per_device",events},{"concurrency",concurrency},{"accepted",accepted.load()},{"failed",failures.load()},{"retries",retry_count.load()},{"seconds",seconds},{"accepted_per_second",accepted.load()/seconds},{"p50_ms",percentile(.5)},{"p95_ms",percentile(.95)},{"p99_ms",percentile(.99)},{"event_bytes_mean",bytes.load()/(static_cast<double>(devices)*events)},{"durability","upstream DDQ fsync per record; PostgreSQL synchronous_commit=on"},{"latency_scope","acceptance including retries; excludes registration and worker drain"}};
        manifest.flush();
        report["response_counts"]=response_counts;report["error_samples"]=error_samples;report["scenario"]=scenario;
        if(!manifest) throw std::runtime_error("cannot write acceptance manifest");
        std::ofstream file(output); if(!file) throw std::runtime_error("cannot open report"); file<<report.dump(2)<<'\n'; std::cout<<report.dump(2)<<'\n'; return failures ? 1 : 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
