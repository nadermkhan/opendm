#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

namespace opendm {

class SpeedLimiter {
public:
    SpeedLimiter();
    ~SpeedLimiter() = default;

    void set_enabled(bool enabled);
    bool is_enabled() const;

    void set_limit_bytes_per_sec(uint64_t limit);
    uint64_t get_limit_bytes_per_sec() const;

    // Called by worker threads after reading/writing data to enforce bandwidth throttling.
    // Optional abort_flag allows immediate wakeup on pause/stop.
    void throttle(size_t bytes_read, const std::atomic<bool>* abort_flag = nullptr);

    void reset();

private:
    mutable std::mutex mutex_;
    std::atomic<bool> enabled_{false};
    std::atomic<uint64_t> limit_bytes_per_sec_{0};
    double available_tokens_{0.0};
    std::chrono::steady_clock::time_point last_refill_;
};

} // namespace opendm
