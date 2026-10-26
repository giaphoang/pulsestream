#pragma once
#include "pulse/model.hpp"
#include "ddq.h"
#include <mutex>
#include <optional>
namespace pulse {
class Queue {
    ddq_t* queue_ = nullptr;
    mutable std::mutex mutex_;
    bool failed_ = false;
    uint32_t lease_ms_;
    void check(int);
public:
    explicit Queue(const std::string& directory, uint32_t max_items = 100000, uint32_t lease_ms = 30000);
    ~Queue();
    Queue(const Queue&) = delete;
    uint64_t enqueue(const Json& envelope);
    std::optional<std::pair<uint64_t,Json>> claim();
    void ack(uint64_t);
    void retry(uint64_t, uint32_t delay_ms);
    ddq_stats_t stats();
    bool failed();
};
}
