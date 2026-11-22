#include "chunk_manager.hpp"
#include "win32_unicode.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>

namespace fs = std::filesystem;

namespace opendm {

ChunkManager::ChunkManager() = default;

bool ChunkManager::init_first_chunk(int64_t total_size, const std::string& temp_dir, const std::string& file_prefix) {
    std::lock_guard<std::mutex> lock(mutex_);
    chunks_.clear();
    total_file_size_ = total_size;
    next_chunk_id_ = 0;
    is_streaming_ = false;

    if (total_size <= 0) {
        return false;
    }

    fs::path td = sys::path_from_utf8(temp_dir.empty() ? "." : temp_dir);
    std::error_code ec;
    fs::create_directories(td, ec);
    if (ec && !fs::exists(td, ec)) {
        return false;
    }

    auto chunk = std::make_shared<Chunk>();
    chunk->id = next_chunk_id_++;
    chunk->start_offset = 0;
    chunk->end_offset = total_size - 1;
    chunk->downloaded_bytes = 0;
    chunk->current_speed = 0;
    chunk->status = ChunkStatus::PENDING;
    chunk->temp_filepath = sys::path_to_utf8(td / sys::path_from_utf8(file_prefix + ".part0"));

    chunks_.push_back(chunk);
    return true;
}

bool ChunkManager::init_stream_chunk(const std::string& temp_dir, const std::string& file_prefix) {
    std::lock_guard<std::mutex> lock(mutex_);
    chunks_.clear();
    total_file_size_ = -1;
    next_chunk_id_ = 0;
    is_streaming_ = true;

    fs::path td = sys::path_from_utf8(temp_dir.empty() ? "." : temp_dir);
    std::error_code ec;
    fs::create_directories(td, ec);
    if (ec && !fs::exists(td, ec)) {
        return false;
    }

    auto chunk = std::make_shared<Chunk>();
    chunk->id = next_chunk_id_++;
    chunk->start_offset = 0;
    chunk->end_offset = -1;
    chunk->downloaded_bytes = 0;
    chunk->current_speed = 0;
    chunk->status = ChunkStatus::PENDING;
    chunk->temp_filepath = sys::path_to_utf8(td / sys::path_from_utf8(file_prefix + ".part0"));

    chunks_.push_back(chunk);
    return true;
}

bool ChunkManager::need_new_connection(size_t current_connections, size_t max_connections) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (is_streaming_ || total_file_size_ <= 0) {
        return false;
    }
    if (current_connections >= max_connections) {
        return false;
    }

    int target_idx = -1;
    int64_t max_free = 0;

    for (size_t i = 0; i < chunks_.size(); ++i) {
        const auto& c = chunks_[i];
        if (c->status == ChunkStatus::DOWNLOADING || c->status == ChunkStatus::PENDING) {
            int64_t free_bytes = c->remaining_bytes();
            if (free_bytes > max_free) {
                max_free = free_bytes;
                target_idx = static_cast<int>(i);
            }
        }
    }

    if (target_idx == -1) {
        return false;
    }

    return (max_free >= 2 * MIN_CHUNK_SIZE + 3);
}

int ChunkManager::find_max_free_chunk() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (is_streaming_) {
        return -1;
    }

    int best_id = -1;
    int64_t max_free = 0;

    for (const auto& c : chunks_) {
        if (c->status == ChunkStatus::DOWNLOADING || c->status == ChunkStatus::PENDING) {
            int64_t free_bytes = c->remaining_bytes();
            if (free_bytes > max_free) {
                max_free = free_bytes;
                best_id = static_cast<int>(c->id);
            }
        }
    }

    return best_id;
}

int ChunkManager::find_chunk_with_zip_central_dir(int64_t target_offset, int64_t* out_remaining_bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (out_remaining_bytes) {
        *out_remaining_bytes = 0;
    }
    for (const auto& c : chunks_) {
        if (c->status == ChunkStatus::DOWNLOADING || c->status == ChunkStatus::PENDING) {
            int64_t current_pos = c->start_offset + c->downloaded_bytes;
            if (current_pos <= target_offset && target_offset <= c->end_offset) {
                if (out_remaining_bytes) {
                    *out_remaining_bytes = c->remaining_bytes();
                }
                return static_cast<int>(c->id);
            }
        }
    }
    return -1;
}

int ChunkManager::split_chunk(int target_chunk_id, const std::string& temp_dir, const std::string& file_prefix) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (is_streaming_) {
        return -1;
    }

    auto it = std::find_if(chunks_.begin(), chunks_.end(), [target_chunk_id](const std::shared_ptr<Chunk>& c) {
        return c->id == static_cast<uint32_t>(target_chunk_id);
    });

    if (it == chunks_.end()) {
        return -1;
    }

    auto target = *it;
    int64_t current_pos = target->start_offset + target->downloaded_bytes;
    int64_t remaining = target->end_offset - current_pos + 1;

    if (remaining < 2 * MIN_CHUNK_SIZE + 3) {
        return -1;
    }

    fs::path td = sys::path_from_utf8(temp_dir.empty() ? "." : temp_dir);
    std::error_code ec;
    fs::create_directories(td, ec);

    int64_t split_point = current_pos + (remaining / 2);
    int64_t original_end = target->end_offset;
    target->end_offset = split_point - 1;

    auto new_chunk = std::make_shared<Chunk>();
    new_chunk->id = next_chunk_id_++;
    new_chunk->start_offset = split_point;
    new_chunk->end_offset = original_end;
    new_chunk->downloaded_bytes = 0;
    new_chunk->current_speed = 0;
    new_chunk->status = ChunkStatus::PENDING;
    new_chunk->temp_filepath = sys::path_to_utf8(td / sys::path_from_utf8(file_prefix + ".part" + std::to_string(new_chunk->id)));

    auto insert_pos = std::upper_bound(chunks_.begin(), chunks_.end(), new_chunk,
        [](const std::shared_ptr<Chunk>& a, const std::shared_ptr<Chunk>& b) {
            return a->start_offset < b->start_offset;
        });
    chunks_.insert(insert_pos, new_chunk);

    return static_cast<int>(new_chunk->id);
}

int ChunkManager::claim_next_pending_chunk() {
    std::lock_guard<std::mutex> lock(mutex_);
    int best_id = -1;
    int64_t max_rem = -1;
    for (const auto& c : chunks_) {
        if (c->status == ChunkStatus::PENDING) {
            int64_t rem = c->remaining_bytes();
            if (best_id == -1 || rem > max_rem) {
                max_rem = rem;
                best_id = static_cast<int>(c->id);
            }
        }
    }
    if (best_id != -1) {
        for (auto& c : chunks_) {
            if (c->id == static_cast<uint32_t>(best_id)) {
                c->status = ChunkStatus::DOWNLOADING;
                break;
            }
        }
    }
    return best_id;
}

void ChunkManager::reconcile_disk_sizes() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (is_streaming_) {
        return;
    }
    for (auto& c : chunks_) {
        if (c->status == ChunkStatus::COMPLETED) {
            continue;
        }
        std::error_code ec;
        fs::path part_p = sys::path_from_utf8(c->temp_filepath);
        int64_t disk_bytes = 0;
        if (!c->temp_filepath.empty() && fs::exists(part_p, ec)) {
            uint64_t sz = fs::file_size(part_p, ec);
            if (!ec) {
                disk_bytes = static_cast<int64_t>(sz);
            }
        }
        int64_t max_sz = c->total_size();
        if (max_sz > 0 && disk_bytes > max_sz) {
            std::error_code r_ec;
            fs::resize_file(part_p, static_cast<uintmax_t>(max_sz), r_ec);
            if (!r_ec) {
                disk_bytes = max_sz;
            }
        }
        c->downloaded_bytes = std::max<int64_t>(0, disk_bytes);
        c->current_speed = 0;
        if (max_sz > 0 && c->downloaded_bytes >= max_sz) {
            c->downloaded_bytes = max_sz;
            c->status = ChunkStatus::COMPLETED;
        } else {
            c->status = ChunkStatus::PENDING;
        }
    }
}

void ChunkManager::update_chunk_progress(int chunk_id, int64_t bytes_received, uint64_t speed_bytes_per_sec) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& c : chunks_) {
        if (c->id == static_cast<uint32_t>(chunk_id)) {
            c->downloaded_bytes = bytes_received;
            c->current_speed = speed_bytes_per_sec;
            if (c->status == ChunkStatus::PENDING) {
                c->status = ChunkStatus::DOWNLOADING;
            }
            break;
        }
    }
}

size_t ChunkManager::clamp_and_update_chunk(
    int chunk_id,
    int64_t current_bytes_received,
    size_t incoming_bytes,
    uint64_t speed_bytes_per_sec,
    bool& out_reached_boundary) {

    std::lock_guard<std::mutex> lock(mutex_);
    out_reached_boundary = false;

    for (auto& c : chunks_) {
        if (c->id == static_cast<uint32_t>(chunk_id)) {
            size_t allowed = incoming_bytes;
            if (!is_streaming_ && c->end_offset >= c->start_offset) {
                int64_t max_bytes = c->end_offset - c->start_offset + 1;
                if (current_bytes_received >= max_bytes) {
                    c->downloaded_bytes = max_bytes;
                    c->current_speed = 0;
                    out_reached_boundary = true;
                    return 0;
                }
                if (current_bytes_received + static_cast<int64_t>(incoming_bytes) >= max_bytes) {
                    allowed = static_cast<size_t>(max_bytes - current_bytes_received);
                    out_reached_boundary = true;
                }
            }
            c->downloaded_bytes = current_bytes_received + static_cast<int64_t>(allowed);
            c->current_speed = speed_bytes_per_sec;
            if (c->status == ChunkStatus::PENDING) {
                c->status = ChunkStatus::DOWNLOADING;
            }
            return allowed;
        }
    }
    return 0;
}

void ChunkManager::rollback_chunk_write(int chunk_id, int64_t committed_bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& c : chunks_) {
        if (c->id == static_cast<uint32_t>(chunk_id)) {
            c->downloaded_bytes = std::max<int64_t>(0, committed_bytes);
            c->current_speed = 0;
            break;
        }
    }
}

void ChunkManager::set_chunk_status(int chunk_id, ChunkStatus status) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& c : chunks_) {
        if (c->id == static_cast<uint32_t>(chunk_id)) {
            c->status = status;
            break;
        }
    }
}

void ChunkManager::mark_chunk_completed(int chunk_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& c : chunks_) {
        if (c->id == static_cast<uint32_t>(chunk_id)) {
            c->status = ChunkStatus::COMPLETED;
            c->current_speed = 0;
            break;
        }
    }
}

bool ChunkManager::all_chunks_completed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (chunks_.empty()) return false;
    for (const auto& c : chunks_) {
        if (c->status != ChunkStatus::COMPLETED) {
            return false;
        }
    }
    return true;
}

int64_t ChunkManager::get_total_downloaded_bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t total = 0;
    for (const auto& c : chunks_) {
        total += c->downloaded_bytes;
    }
    return total;
}

int64_t ChunkManager::get_total_size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_file_size_;
}

std::vector<Chunk> ChunkManager::get_chunks_snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Chunk> result;
    result.reserve(chunks_.size());
    for (const auto& c : chunks_) {
        result.push_back(*c);
    }
    return result;
}

bool ChunkManager::get_chunk(int chunk_id, Chunk& out_chunk) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& c : chunks_) {
        if (c->id == static_cast<uint32_t>(chunk_id)) {
            out_chunk = *c;
            return true;
        }
    }
    return false;
}

void ChunkManager::reset_all_speeds() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& c : chunks_) {
        c->current_speed = 0;
    }
}

int64_t ChunkManager::get_remaining_bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t rem = 0;
    for (const auto& c : chunks_) {
        if (c->status != ChunkStatus::COMPLETED && c->end_offset >= c->start_offset) {
            rem += c->remaining_bytes();
        }
    }
    return rem;
}

bool ChunkManager::join_file(const std::string& destination_filepath,
                             std::function<void(double)> progress_cb,
                             const std::atomic<bool>* cancel_flag) {
    std::vector<Chunk> sorted_chunks;
    int64_t total_size = 0;
    bool streaming = false;
    std::string temp_dir_to_clean;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (chunks_.empty()) {
            return false;
        }
        for (const auto& c : chunks_) {
            sorted_chunks.push_back(*c);
            if (temp_dir_to_clean.empty() && !c->temp_filepath.empty()) {
                fs::path tp = sys::path_from_utf8(c->temp_filepath);
                temp_dir_to_clean = sys::path_to_utf8(tp.parent_path());
            }
        }
        total_size = total_file_size_;
        streaming = is_streaming_;
    }

    std::sort(sorted_chunks.begin(), sorted_chunks.end(),
        [](const Chunk& a, const Chunk& b) {
            return a.start_offset < b.start_offset;
        });

    std::error_code ec;
    int64_t expected_total = 0;
    for (const auto& chunk : sorted_chunks) {
        fs::path part_p = sys::path_from_utf8(chunk.temp_filepath);
        if (!fs::exists(part_p, ec)) {
            return false;
        }
        uint64_t disk_sz = fs::file_size(part_p, ec);
        if (ec) return false;

        if (!streaming && chunk.end_offset >= chunk.start_offset) {
            int64_t expected_chunk_sz = chunk.end_offset - chunk.start_offset + 1;
            if (static_cast<int64_t>(disk_sz) < expected_chunk_sz) {
                return false;
            }
            expected_total += expected_chunk_sz;
        } else {
            expected_total += static_cast<int64_t>(disk_sz);
        }
    }

    if (total_size <= 0) {
        total_size = expected_total;
    }

    fs::path dest_p = sys::path_from_utf8(destination_filepath);
    if (dest_p.has_parent_path()) {
        fs::create_directories(dest_p.parent_path(), ec);
    }

    // Fast path for single-chunk downloads on the same filesystem: zero copy overhead
    if (sorted_chunks.size() == 1) {
        fs::path part_p = sys::path_from_utf8(sorted_chunks[0].temp_filepath);
        if (!streaming && sorted_chunks[0].end_offset >= sorted_chunks[0].start_offset) {
            uint64_t exact_sz = static_cast<uint64_t>(sorted_chunks[0].end_offset - sorted_chunks[0].start_offset + 1);
            if (fs::file_size(part_p, ec) != exact_sz) {
                fs::resize_file(part_p, exact_sz, ec);
                if (ec) {
                    return false;
                }
            }
        }
        if (sys::atomic_replace_file(part_p, dest_p, ec)) {
            if (!temp_dir_to_clean.empty()) {
                fs::remove(sys::path_from_utf8(temp_dir_to_clean), ec);
            }
            if (progress_cb) progress_cb(1.0);
            return true;
        }
    }

    std::string staging_utf8 = destination_filepath + ".assembling.tmp";
    fs::path staging_p = sys::path_from_utf8(staging_utf8);
    std::ofstream dest_file(staging_p, std::ios::binary | std::ios::trunc);
    if (!dest_file.is_open()) {
        return false;
    }

    constexpr size_t BUFFER_SIZE = 4 * 1024 * 1024;
    std::vector<char> io_buf(BUFFER_SIZE);
    std::vector<char> out_stream_buf(BUFFER_SIZE);
    dest_file.rdbuf()->pubsetbuf(out_stream_buf.data(), static_cast<std::streamsize>(out_stream_buf.size()));

    int64_t total_written = 0;
    auto last_cb_time = std::chrono::steady_clock::now();
    double last_reported_progress = -1.0;

    for (const auto& chunk : sorted_chunks) {
        if (cancel_flag && cancel_flag->load(std::memory_order_relaxed)) {
            dest_file.close();
            fs::remove(staging_p, ec);
            return false;
        }

        std::ifstream part_file(sys::path_from_utf8(chunk.temp_filepath), std::ios::binary);
        if (!part_file.is_open()) {
            dest_file.close();
            fs::remove(staging_p, ec);
            return false;
        }

        int64_t bytes_remaining_for_chunk = (!streaming && chunk.end_offset >= chunk.start_offset)
            ? (chunk.end_offset - chunk.start_offset + 1)
            : std::numeric_limits<int64_t>::max();

        while (bytes_remaining_for_chunk > 0 && part_file.good()) {
            if (cancel_flag && cancel_flag->load(std::memory_order_relaxed)) {
                part_file.close();
                dest_file.close();
                fs::remove(staging_p, ec);
                return false;
            }

            size_t to_read = static_cast<size_t>(
                std::min<int64_t>(static_cast<int64_t>(io_buf.size()), bytes_remaining_for_chunk));
            part_file.read(io_buf.data(), static_cast<std::streamsize>(to_read));
            std::streamsize bytes_read = part_file.gcount();
            if (bytes_read <= 0) {
                break;
            }

            dest_file.write(io_buf.data(), bytes_read);
            if (dest_file.fail()) {
                part_file.close();
                dest_file.close();
                fs::remove(staging_p, ec);
                return false;
            }

            bytes_remaining_for_chunk -= bytes_read;
            total_written += bytes_read;

            if (progress_cb && total_size > 0) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_cb_time).count();
                double progress = std::min(1.0, static_cast<double>(total_written) / total_size);
                if (elapsed_ms >= 50 || (progress - last_reported_progress) >= 0.01) {
                    progress_cb(progress);
                    last_cb_time = now;
                    last_reported_progress = progress;
                }
            }
        }

        part_file.close();

        if (!streaming && chunk.end_offset >= chunk.start_offset && bytes_remaining_for_chunk != 0) {
            dest_file.close();
            fs::remove(staging_p, ec);
            return false;
        }
    }

    dest_file.flush();
    bool write_ok = dest_file.good();
    dest_file.close();

    if (!write_ok) {
        fs::remove(staging_p, ec);
        return false;
    }

    if (!sys::atomic_replace_file(staging_p, dest_p, ec)) {
        fs::remove(staging_p, ec);
        return false;
    }

    for (const auto& chunk : sorted_chunks) {
        fs::remove(sys::path_from_utf8(chunk.temp_filepath), ec);
    }
    if (!temp_dir_to_clean.empty()) {
        fs::remove(sys::path_from_utf8(temp_dir_to_clean), ec);
    }

    if (progress_cb) {
        progress_cb(1.0);
    }
    return true;
}

void ChunkManager::cleanup_temp_files() {
    std::lock_guard<std::mutex> lock(mutex_);
    fs::path temp_dir;
    for (const auto& c : chunks_) {
        if (!c->temp_filepath.empty()) {
            std::error_code ec;
            fs::path tp = sys::path_from_utf8(c->temp_filepath);
            fs::remove(tp, ec);
            if (temp_dir.empty()) {
                temp_dir = tp.parent_path();
            }
        }
    }
    if (!temp_dir.empty()) {
        std::error_code ec;
        fs::remove(temp_dir, ec);
        // Clean empty parent .opendm_tmp container if no other session temp dirs remain inside it
        if (temp_dir.parent_path().filename() == ".opendm_tmp") {
            fs::remove(temp_dir.parent_path(), ec);
        }
    }
}

void ChunkManager::reset() {
    cleanup_temp_files();
    std::lock_guard<std::mutex> lock(mutex_);
    chunks_.clear();
    total_file_size_ = -1;
    next_chunk_id_ = 0;
    is_streaming_ = false;
}

std::string ChunkManager::serialize_state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json chunks_arr = nlohmann::json::array();
    for (const auto& c : chunks_) {
        chunks_arr.push_back({
            {"id", c->id},
            {"start_offset", c->start_offset},
            {"end_offset", c->end_offset},
            {"downloaded_bytes", c->downloaded_bytes},
            {"status", static_cast<int>(c->status)},
            {"temp_filepath", c->temp_filepath}
        });
    }
    nlohmann::json root = {
        {"total_file_size", total_file_size_},
        {"is_streaming", is_streaming_},
        {"next_chunk_id", next_chunk_id_},
        {"chunks", std::move(chunks_arr)}
    };
    return root.dump(2);
}

bool ChunkManager::deserialize_state(const std::string& json_str) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(json_str);
    } catch (...) {
        return false;
    }
    if (!j.is_object()) return false;

    std::vector<std::shared_ptr<Chunk>> loaded;
    int64_t t_size = j.value("total_file_size", int64_t{-1});
    bool streaming = j.value("is_streaming", false);
    int64_t next_id_raw = j.value("next_chunk_id", int64_t{0});
    uint32_t max_seen_id = 0;

    if (j.contains("chunks") && j["chunks"].is_array()) {
        for (const auto& item : j["chunks"]) {
            if (!item.is_object() || !item.contains("id")) continue;
            auto chunk = std::make_shared<Chunk>();
            chunk->id = item.value("id", 0U);
            chunk->start_offset = item.value("start_offset", int64_t{0});
            chunk->end_offset = item.value("end_offset", int64_t{0});
            chunk->downloaded_bytes = item.value("downloaded_bytes", int64_t{0});
            int st = item.value("status", 0);
            if (st < 0 || st > 3) st = 0;
            chunk->status = static_cast<ChunkStatus>(st);
            chunk->temp_filepath = item.value("temp_filepath", "");
            if (chunk->id >= max_seen_id) {
                max_seen_id = chunk->id + 1;
            }
            loaded.push_back(std::move(chunk));
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    chunks_ = std::move(loaded);
    total_file_size_ = t_size;
    is_streaming_ = streaming;
    next_chunk_id_ = (next_id_raw >= 0)
        ? std::max<uint32_t>(static_cast<uint32_t>(next_id_raw), max_seen_id)
        : max_seen_id;
    return !chunks_.empty();
}

} // namespace opendm
