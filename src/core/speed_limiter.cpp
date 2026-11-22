#include "speed_limiter.hpp"

#include <algorithm>
#include <thread>

namespace opendm {

SpeedLimiter::SpeedLimiter()
    : last_refill_(std::chrono::steady_clock::now()) {}

void SpeedLimiter::set_enabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    enabled_.store(enabled, std::memory_order_relaxed);
    reset();
}

bool SpeedLimiter::is_enabled() const {
    return enabled_.load(std::memory_order_relaxed);
}

void SpeedLimiter::set_limit_bytes_per_sec(uint64_t limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    limit_bytes_per_sec_.store(limit, std::memory_order_relaxed);
    reset();
}

uint64_t SpeedLimiter::get_limit_bytes_per_sec() const {
    return limit_bytes_per_sec_.load(std::memory_order_relaxed);
}

void SpeedLimiter::reset() {
    available_tokens_ = 0.0;
    last_refill_ = std::chrono::steady_clock::now();
}

void SpeedLimiter::throttle(size_t bytes_read, const std::atomic<bool>* abort_flag) {
    if (!enabled_.load(std::memory_order_relaxed) ||
        limit_bytes_per_sec_.load(std::memory_order_relaxed) == 0 ||
        bytes_read == 0) {
        return;
    }

    int64_t sleep_ms = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        uint64_t limit = limit_bytes_per_sec_.load(std::memory_order_relaxed);
        if (!enabled_.load(std::memory_order_relaxed) || limit == 0) {
            return;
        }

        auto now = std::chrono::steady_clock::now();
        double elapsed_sec = std::chrono::duration<double>(now - last_refill_).count();
        if (elapsed_sec > 0.0) {
            double max_burst = std::clamp(static_cast<double>(limit) * 0.25, 32768.0, 524288.0);
            available_tokens_ = std::min(max_burst, available_tokens_ + elapsed_sec * static_cast<double>(limit));
            last_refill_ = now;
        }

        available_tokens_ -= static_cast<double>(bytes_read);

        // Cap token debt so simultaneous large worker reads do not accumulate >2s sleeps
        // that would trigger libcurl's LOW_SPEED_TIME timeout.
        double max_debt = -static_cast<double>(limit) * 1.5;
        if (available_tokens_ < max_debt) {
            available_tokens_ = max_debt;
        }

        if (available_tokens_ < 0.0) {
            sleep_ms = static_cast<int64_t>((-available_tokens_ * 1000.0) / static_cast<double>(limit));
            sleep_ms = std::clamp<int64_t>(sleep_ms, 1, 1500);
        }
    }

    while (sleep_ms > 0) {
        if (abort_flag && abort_flag->load(std::memory_order_relaxed)) {
            break;
        }
        int64_t step = std::min<int64_t>(sleep_ms, 25);
        std::this_thread::sleep_for(std::chrono::milliseconds(step));
        sleep_ms -= step;
    }
}

} // namespace opendm
