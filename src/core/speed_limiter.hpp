#pragma once

#include <cstdint>
#include <chrono>
#include <mutex>
#include <thread>

namespace opendm {

class SpeedLimiter {
public:
    SpeedLimiter();
    ~SpeedLimiter() = default;

    // Enable or disable speed limit
    void set_enabled(bool enabled);
    bool is_enabled() const;

    // Set speed limit in bytes per second (e.g. 500 * 1024 for 500 KB/s)
    void set_limit_bytes_per_sec(uint64_t limit);
    uint64_t get_limit_bytes_per_sec() const;

    // Called by worker threads after reading data to enforce bandwidth throttling
    void throttle(size_t bytes_read);

    // Reset throttle tracking window
    void reset();

private:
    mutable std::mutex mutex_;
    bool enabled_{false};
    uint64_t limit_bytes_per_sec_{0};
    uint64_t bytes_in_current_window_{0};
    std::chrono::steady_clock::time_point window_start_;
};

} // namespace opendm
