#include "speed_limiter.hpp"

namespace opendm {

SpeedLimiter::SpeedLimiter()
    : window_start_(std::chrono::steady_clock::now()) {}

void SpeedLimiter::set_enabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    enabled_ = enabled;
    reset();
}

bool SpeedLimiter::is_enabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return enabled_;
}

void SpeedLimiter::set_limit_bytes_per_sec(uint64_t limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    limit_bytes_per_sec_ = limit;
    reset();
}

uint64_t SpeedLimiter::get_limit_bytes_per_sec() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return limit_bytes_per_sec_;
}

void SpeedLimiter::reset() {
    bytes_in_current_window_ = 0;
    window_start_ = std::chrono::steady_clock::now();
}

void SpeedLimiter::throttle(size_t bytes_read) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!enabled_ || limit_bytes_per_sec_ == 0 || bytes_read == 0) {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - window_start_).count();

    if (elapsed_ms >= 1000) {
        // Reset 1-second rolling window
        bytes_in_current_window_ = bytes_read;
        window_start_ = now;
        return;
    }

    bytes_in_current_window_ += bytes_read;
    // Expected time (in ms) to transmit bytes_in_current_window_ at limit_bytes_per_sec_
    int64_t expected_ms = (bytes_in_current_window_ * 1000) / limit_bytes_per_sec_;

    if (expected_ms > elapsed_ms) {
        int64_t sleep_ms = expected_ms - elapsed_ms;
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
    }
}

} // namespace opendm
