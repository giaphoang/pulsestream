#include "pulse/model.hpp"
#include "pulse/queue.hpp"
#include "pulse/http.hpp"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unistd.h>
using namespace pulse;
namespace {
Json event() { return {{"device_id","synthetic-1"},{"event_id","event-1"},{"schema_version",1},{"timestamp_ms",1000},{"sequence",1},{"type","heartbeat"},{"battery_pct",80}}; }
struct Temp {
    std::string path;
    Temp() { char p[]="/tmp/pulsestream-unit-XXXXXX"; auto out=mkdtemp(p); if(!out) throw std::runtime_error("mkdtemp"); path=out; }
    ~Temp() { std::filesystem::remove_all(path); }
};
}
TEST(Model, HeartbeatRetainsProvenance) { auto e=event(); EXPECT_EQ(validate_event(e,1000),e); }
TEST(Model, DiagnosticEvent) { auto e=event(); e.erase("battery_pct"); e["type"]="diagnostic_event"; e["diagnostic_flag"]="synthetic_flag"; EXPECT_EQ(validate_event(e,1000),e); }
TEST(Model, ScheduledPart) { auto e=event(); e.erase("battery_pct"); e["type"]="scheduled_transmission"; e["transmission_id"]="t1"; e["part_index"]=0; e["parts_total"]=2; EXPECT_EQ(validate_event(e,1000),e); }
TEST(Model, DeviceStatus) { auto e=event(); e.erase("battery_pct"); e["type"]="device_status"; e["status"]="reconnect"; EXPECT_EQ(validate_event(e,1000),e); }
TEST(Model, BatteryBoundaries) { for(int b:{0,100}) {auto e=event(); e["battery_pct"]=b; EXPECT_NO_THROW(validate_event(e,1000));} }
TEST(Model, OldEventAllowed) { EXPECT_NO_THROW(validate_event(event(),9999999)); }
TEST(Model, CanonicalFieldOrder) { EXPECT_EQ(Json::parse("{\"b\":1,\"a\":2}").dump(),Json::parse("{\"a\":2,\"b\":1}").dump()); }
TEST(Model, CredentialHash) { EXPECT_EQ(digest("abc"),"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"); }
TEST(Model, CredentialComparison) { EXPECT_TRUE(secret_equal("same","same")); EXPECT_FALSE(secret_equal("same","samo")); EXPECT_FALSE(secret_equal("same","sam")); }
class BadEvent : public ::testing::TestWithParam<Json> {};
TEST_P(BadEvent, RejectsInvalidField) { auto e=event(); e.update(GetParam()); EXPECT_THROW(validate_event(e,1000),Error); }
INSTANTIATE_TEST_SUITE_P(Validation,BadEvent,::testing::Values(
    Json{{"device_id",""}},Json{{"device_id","../x"}},Json{{"device_id",std::string(65,'a')}},Json{{"device_id",42}},
    Json{{"event_id",nullptr}},Json{{"event_id","contains space"}},Json{{"schema_version",2}},Json{{"schema_version",1.0}},
    Json{{"sequence",0}},Json{{"sequence",-1}},Json{{"sequence",1.5}},Json{{"sequence",UINT64_MAX}},
    Json{{"timestamp_ms",0}},Json{{"timestamp_ms",301001}},Json{{"timestamp_ms","1000"}},
    Json{{"battery_pct",-1}},Json{{"battery_pct",101}},Json{{"battery_pct",true}},Json{{"type","ecg_control"}},
    Json{{"diagnostic_flag","wrong_type_field"}},Json{{"unexpected",1}}
));
TEST(Model, MissingRequiredFields) { for(auto key:{"device_id","event_id","schema_version","timestamp_ms","sequence","type","battery_pct"}) {auto e=event();e.erase(key);EXPECT_THROW(validate_event(e,1000),Error);} }
TEST(Model, WrongContainer) { EXPECT_THROW(validate_event(Json::array(),1000),Error); }
TEST(Model, ScheduledBounds) { auto e=event();e.erase("battery_pct");e["type"]="scheduled_transmission";e["transmission_id"]="t1";e["part_index"]=2;e["parts_total"]=2;EXPECT_THROW(validate_event(e,1000),Error); }
TEST(Model, DeviceStatusEnum) { auto e=event();e.erase("battery_pct");e["type"]="device_status";e["status"]="control_pacing";EXPECT_THROW(validate_event(e,1000),Error); }
TEST(Tracking, Expected) { EXPECT_EQ(transmission_state(0,2,1000,0,999),"expected"); }
TEST(Tracking, Incomplete) { EXPECT_EQ(transmission_state(1,2,1000,900,999),"incomplete"); }
TEST(Tracking, Overdue) { EXPECT_EQ(transmission_state(1,2,1000,900,1001),"overdue"); }
TEST(Tracking, Received) { EXPECT_EQ(transmission_state(2,2,1000,1000,2000),"received"); }
TEST(Tracking, Delayed) { EXPECT_EQ(transmission_state(2,2,1000,1001,2000),"delayed"); }
TEST(Queue, RestartPreservesPayload) { Temp t; {Queue q(t.path);q.enqueue(event());} Queue q(t.path);auto item=q.claim();ASSERT_TRUE(item);EXPECT_EQ(item->second,event());q.ack(item->first);EXPECT_FALSE(q.claim()); }
TEST(Queue, AckSurvivesRestart) {Temp t;{Queue q(t.path);auto id=q.enqueue(event());q.claim();q.ack(id);}Queue q(t.path);EXPECT_FALSE(q.claim());}
TEST(Queue, RetryReleasesClaim) {Temp t;Queue q(t.path);auto id=q.enqueue(event());q.claim();q.retry(id,0);auto item=q.claim();ASSERT_TRUE(item);EXPECT_EQ(item->first,id);}
TEST(Queue, ExclusiveDirectoryOwnership) {Temp t;Queue q(t.path);EXPECT_THROW(Queue second(t.path),Error);}
TEST(Queue, CapacityIsBounded) {Temp t;Queue q(t.path,1);q.enqueue(event());EXPECT_THROW(q.enqueue(event()),Error);EXPECT_FALSE(q.failed());}
TEST(Queue, ConcurrentProducers) {Temp t;Queue q(t.path);std::vector<std::thread> threads;for(int i=0;i<4;++i)threads.emplace_back([&]{for(int k=0;k<5;++k)q.enqueue(event());});for(auto& th:threads)th.join();EXPECT_EQ(q.stats().available_items,20u);}
TEST(Queue, OversizedPayloadRejected) {Temp t;Queue q(t.path);EXPECT_THROW(q.enqueue(Json{{"blob",std::string(20000,'x')}}),Error);EXPECT_FALSE(q.failed());}
TEST(HTTP, ParsesBodyLength) {size_t length=0;auto r=parse_http_head("POST /v1/events HTTP/1.1\r\nHost: local\r\nContent-Type: application/json\r\nContent-Length: 42",length);EXPECT_EQ(length,42u);EXPECT_EQ(r.method,"POST");}
class BadHTTP : public ::testing::TestWithParam<std::string> {};
TEST_P(BadHTTP, RejectsAmbiguousFraming) {size_t length=0;EXPECT_THROW(parse_http_head(GetParam(),length),Error);}
INSTANTIATE_TEST_SUITE_P(Framing,BadHTTP,::testing::Values(
    "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 1",
    "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: -1",
    "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 20000",
    "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked",
    "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\ncontent-length: 0",
    "GET / HTTP/1.0\r\nHost: x",
    "GET / HTTP/1.1\r\nAuthorization: x",
    "POST / HTTP/1.1\r\nHost: x",
    "GET / HTTP/1.1\r\nHost: x\r\nBad Header: x"
));
