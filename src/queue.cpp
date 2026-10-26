#include "pulse/queue.hpp"
#include <filesystem>
namespace pulse {
void Queue::check(int s) {
    if(s == DDQ_OK) return;
    if(s != DDQ_FULL && s != DDQ_TOO_LARGE) failed_=true;
    throw Error(503,std::string("durable queue: ") + ddq_strerror(s));
}
Queue::Queue(const std::string& path, uint32_t limit, uint32_t lease):lease_ms_(lease) {
    std::filesystem::create_directories(path); ddq_options_t o; ddq_options_default(&o);
    o.max_items=limit; o.max_payload_bytes=16384; o.segment_bytes=8*1024*1024; o.max_segments=64;
    check(ddq_open(&queue_,path.c_str(),&o));
}
Queue::~Queue() { if(queue_) ddq_close(queue_); }
uint64_t Queue::enqueue(const Json& j) {
    std::lock_guard lock(mutex_); if(failed_) throw Error(503,"queue requires restart");
    auto payload=j.dump(); uint64_t id; check(ddq_enqueue(queue_,payload.data(),payload.size(),&id)); return id;
}
std::optional<std::pair<uint64_t,Json>> Queue::claim() {
    std::lock_guard lock(mutex_); if(failed_) throw Error(503,"queue requires restart");
    ddq_claim_t c; auto s=ddq_claim(queue_,lease_ms_,&c); if(s == DDQ_EMPTY) return std::nullopt; check(s);
    std::string payload(c.payload_len,'\0'); size_t n=0; check(ddq_read(queue_,c.seq,payload.data(),payload.size(),&n));
    return std::pair{c.seq,Json::parse(payload)};
}
void Queue::ack(uint64_t id) { std::lock_guard lock(mutex_); check(ddq_ack(queue_,id)); }
void Queue::retry(uint64_t id,uint32_t delay) { std::lock_guard lock(mutex_); check(ddq_retry(queue_,id,delay)); }
ddq_stats_t Queue::stats() { std::lock_guard lock(mutex_); ddq_stats_t s{}; check(ddq_stats(queue_,&s)); return s; }
bool Queue::failed() { std::lock_guard lock(mutex_); return failed_; }
}
