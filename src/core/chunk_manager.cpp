#include "chunk_manager.hpp"
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <sstream>
#include <iostream>
#include <chrono>
#include <limits>

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

    std::filesystem::create_directories(temp_dir);

    auto chunk = std::make_shared<Chunk>();
    chunk->id = next_chunk_id_++;
    chunk->start_offset = 0;
    chunk->end_offset = total_size - 1;
    chunk->downloaded_bytes = 0;
    chunk->current_speed = 0;
    chunk->status = ChunkStatus::PENDING;
    chunk->temp_filepath = (std::filesystem::path(temp_dir) / (file_prefix + ".part0")).string();

    chunks_.push_back(chunk);
    return true;
}

bool ChunkManager::init_stream_chunk(const std::string& temp_dir, const std::string& file_prefix) {
    std::lock_guard<std::mutex> lock(mutex_);
    chunks_.clear();
    total_file_size_ = -1;
    next_chunk_id_ = 0;
    is_streaming_ = true;

    std::filesystem::create_directories(temp_dir);

    auto chunk = std::make_shared<Chunk>();
    chunk->id = next_chunk_id_++;
    chunk->start_offset = 0;
    chunk->end_offset = -1; // indefinite
    chunk->downloaded_bytes = 0;
    chunk->current_speed = 0;
    chunk->status = ChunkStatus::PENDING;
    chunk->temp_filepath = (std::filesystem::path(temp_dir) / (file_prefix + ".part0")).string();

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

    // IDM algorithm: Check if the chunk with maximum free bytes has >= 2 * MIN_CHUNK_SIZE
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

    // IDM algorithm (IDMan.exe @ 0x0055aeea):
    // Checks if max_free >= 2 * MinChunkSize + 3 (204,803 bytes)
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
        if (c->status == ChunkStatus::DOWNLOADING) {
            int64_t free_bytes = c->remaining_bytes();
            if (free_bytes > max_free) {
                max_free = free_bytes;
                best_id = static_cast<int>(c->id);
            }
        }
    }

    // If no downloading chunk found, check pending chunks
    if (best_id == -1) {
        for (const auto& c : chunks_) {
            if (c->status == ChunkStatus::PENDING) {
                int64_t free_bytes = c->remaining_bytes();
                if (free_bytes > max_free) {
                    max_free = free_bytes;
                    best_id = static_cast<int>(c->id);
                }
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
    // IDM algorithm: ChunksList::FindChunkWithZipCentralDir (IDMan.exe @ 0x005599c0)
    // Searches chunks where start_offset + downloaded_bytes <= target_offset <= end_offset
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

    // IDM threshold: (2 * MinChunkSize + 3) = 204,803 bytes
    if (remaining < 2 * MIN_CHUNK_SIZE + 3) {
        return -1; // Too small to split profitably
    }

    // Carve at midpoint of remaining range
    int64_t split_point = current_pos + (remaining / 2);

    int64_t original_end = target->end_offset;
    target->end_offset = split_point - 1; // Update existing chunk boundary dynamically

    // Create new chunk covering [split_point, original_end]
    auto new_chunk = std::make_shared<Chunk>();
    new_chunk->id = next_chunk_id_++;
    new_chunk->start_offset = split_point;
    new_chunk->end_offset = original_end;
    new_chunk->downloaded_bytes = 0;
    new_chunk->current_speed = 0;
    new_chunk->status = ChunkStatus::PENDING;

    std::filesystem::path p = std::filesystem::path(temp_dir) / (file_prefix + ".part" + std::to_string(new_chunk->id));
    new_chunk->temp_filepath = p.string();

    // Insert sorted by start_offset
    auto insert_pos = std::upper_bound(chunks_.begin(), chunks_.end(), new_chunk,
        [](const std::shared_ptr<Chunk>& a, const std::shared_ptr<Chunk>& b) {
            return a->start_offset < b->start_offset;
        });
    chunks_.insert(insert_pos, new_chunk);

    return static_cast<int>(new_chunk->id);
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

bool ChunkManager::join_file(const std::string& destination_filepath, std::function<void(double)> progress_cb) {
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
                std::filesystem::path tp(c->temp_filepath);
                temp_dir_to_clean = tp.parent_path().string();
            }
        }
        total_size = total_file_size_;
        streaming = is_streaming_;
    }

    // Sort chunks by start_offset
    std::sort(sorted_chunks.begin(), sorted_chunks.end(),
        [](const Chunk& a, const Chunk& b) {
            return a.start_offset < b.start_offset;
        });

    namespace fs = std::filesystem;
    std::error_code ec;

    // Verify all chunk files exist and have sufficient bytes before touching destination
    int64_t expected_total = 0;
    for (size_t i = 0; i < sorted_chunks.size(); ++i) {
        const auto& chunk = sorted_chunks[i];
        if (!fs::exists(chunk.temp_filepath, ec)) {
            return false;
        }
        uint64_t disk_sz = fs::file_size(chunk.temp_filepath, ec);
        if (ec) return false;

        if (!streaming && chunk.end_offset >= chunk.start_offset) {
            int64_t expected_chunk_sz = chunk.end_offset - chunk.start_offset + 1;
            if (static_cast<int64_t>(disk_sz) < expected_chunk_sz) {
                return false; // Incomplete or truncated part file
            }
            expected_total += expected_chunk_sz;
        } else {
            expected_total += static_cast<int64_t>(disk_sz);
        }
    }

    if (total_size <= 0) {
        total_size = expected_total;
    }

    // Ensure parent directory exists
    fs::path dest_p(destination_filepath);
    if (dest_p.has_parent_path()) {
        fs::create_directories(dest_p.parent_path(), ec);
    }

    // Fast-path: if there is only 1 chunk, clamp its size in-place and rename O(1) if on same mount
    if (sorted_chunks.size() == 1) {
        const auto& single = sorted_chunks[0];
        if (!streaming && single.end_offset >= single.start_offset) {
            uint64_t exact_sz = static_cast<uint64_t>(single.end_offset - single.start_offset + 1);
            if (fs::file_size(single.temp_filepath, ec) != exact_sz) {
                fs::resize_file(single.temp_filepath, exact_sz, ec);
            }
        }
        fs::remove(destination_filepath, ec);
        fs::rename(single.temp_filepath, destination_filepath, ec);
        if (!ec) {
            if (!temp_dir_to_clean.empty()) {
                fs::remove(temp_dir_to_clean, ec);
            }
            if (progress_cb) progress_cb(1.0);
            return true;
        }
    }

    // Assemble into a staging file first so failed assembly never destroys destination or parts
    std::string staging_path = destination_filepath + ".assembling.tmp";
    std::ofstream dest_file(staging_path, std::ios::binary | std::ios::trunc);
    if (!dest_file.is_open()) {
        return false;
    }

    // Use a 4 MB stream buffer for high-throughput sequential I/O
    constexpr size_t BUFFER_SIZE = 4 * 1024 * 1024;
    std::vector<char> io_buf(BUFFER_SIZE);
    std::vector<char> out_stream_buf(BUFFER_SIZE);
    dest_file.rdbuf()->pubsetbuf(out_stream_buf.data(), out_stream_buf.size());

    int64_t total_written = 0;
    auto last_cb_time = std::chrono::steady_clock::now();
    double last_reported_progress = -1.0;

    for (size_t i = 0; i < sorted_chunks.size(); ++i) {
        const auto& chunk = sorted_chunks[i];
        std::ifstream part_file(chunk.temp_filepath, std::ios::binary);
        if (!part_file.is_open()) {
            dest_file.close();
            fs::remove(staging_path, ec);
            return false;
        }

        int64_t bytes_remaining_for_chunk = (!streaming && chunk.end_offset >= chunk.start_offset)
            ? (chunk.end_offset - chunk.start_offset + 1)
            : std::numeric_limits<int64_t>::max();

        while (bytes_remaining_for_chunk > 0 && part_file.good()) {
            size_t to_read = static_cast<size_t>(
                std::min<int64_t>(static_cast<int64_t>(io_buf.size()), bytes_remaining_for_chunk));
            part_file.read(io_buf.data(), to_read);
            std::streamsize bytes_read = part_file.gcount();
            if (bytes_read <= 0) {
                break;
            }

            dest_file.write(io_buf.data(), bytes_read);
            if (dest_file.fail()) {
                part_file.close();
                dest_file.close();
                fs::remove(staging_path, ec);
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

        // Ensure non-streaming chunk yielded all expected bytes
        if (!streaming && chunk.end_offset >= chunk.start_offset && bytes_remaining_for_chunk != 0) {
            dest_file.close();
            fs::remove(staging_path, ec);
            return false;
        }
    }

    dest_file.flush();
    bool write_ok = dest_file.good();
    dest_file.close();

    if (!write_ok) {
        fs::remove(staging_path, ec);
        return false;
    }

    // Atomically move staging file to final destination
    fs::remove(destination_filepath, ec);
    fs::rename(staging_path, destination_filepath, ec);
    if (ec) {
        fs::remove(staging_path, ec);
        return false;
    }

    // Only delete temporary .part files AFTER assembly is completely verified
    for (const auto& chunk : sorted_chunks) {
        fs::remove(chunk.temp_filepath, ec);
    }
    if (!temp_dir_to_clean.empty()) {
        fs::remove(temp_dir_to_clean, ec);
    }

    if (progress_cb) {
        progress_cb(1.0);
    }
    return true;
}


void ChunkManager::cleanup_temp_files() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string temp_dir;
    for (const auto& c : chunks_) {
        if (!c->temp_filepath.empty()) {
            std::error_code ec;
            std::filesystem::remove(c->temp_filepath, ec);
            if (temp_dir.empty()) {
                std::filesystem::path tp(c->temp_filepath);
                temp_dir = tp.parent_path().string();
            }
        }
    }
    if (!temp_dir.empty()) {
        std::error_code ec;
        std::filesystem::remove(temp_dir, ec);
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
    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"total_file_size\": " << total_file_size_ << ",\n";
    ss << "  \"is_streaming\": " << (is_streaming_ ? "true" : "false") << ",\n";
    ss << "  \"next_chunk_id\": " << next_chunk_id_ << ",\n";
    ss << "  \"chunks\": [\n";

    for (size_t i = 0; i < chunks_.size(); ++i) {
        const auto& c = chunks_[i];
        ss << "    {\n";
        ss << "      \"id\": " << c->id << ",\n";
        ss << "      \"start_offset\": " << c->start_offset << ",\n";
        ss << "      \"end_offset\": " << c->end_offset << ",\n";
        ss << "      \"downloaded_bytes\": " << c->downloaded_bytes << ",\n";
        ss << "      \"status\": " << static_cast<int>(c->status) << ",\n";
        ss << "      \"temp_filepath\": \"" << c->temp_filepath << "\"\n";
        ss << "    }" << (i + 1 < chunks_.size() ? "," : "") << "\n";
    }

    ss << "  ]\n";
    ss << "}\n";
    return ss.str();
}

bool ChunkManager::deserialize_state(const std::string& json_str) {
    std::lock_guard<std::mutex> lock(mutex_);
    chunks_.clear();

    // Helper lambda to find value by key in simple JSON
    auto get_num = [&json_str](const std::string& key) -> int64_t {
        size_t pos = json_str.find("\"" + key + "\":");
        if (pos == std::string::npos) return -1;
        pos += key.length() + 3;
        while (pos < json_str.length() && (json_str[pos] == ' ' || json_str[pos] == '\t')) pos++;
        try {
            return std::stoll(json_str.substr(pos));
        } catch (...) {
            return -1;
        }
    };

    total_file_size_ = get_num("total_file_size");
    next_chunk_id_ = static_cast<uint32_t>(get_num("next_chunk_id"));
    is_streaming_ = (json_str.find("\"is_streaming\": true") != std::string::npos);

    // Parse chunk objects
    size_t chunk_pos = json_str.find("\"chunks\":");
    if (chunk_pos == std::string::npos) return false;

    size_t cur = chunk_pos;
    while ((cur = json_str.find('{', cur)) != std::string::npos) {
        size_t end = json_str.find('}', cur);
        if (end == std::string::npos) break;
        std::string block = json_str.substr(cur, end - cur + 1);

        auto get_block_num = [&block](const std::string& key) -> int64_t {
            size_t pos = block.find("\"" + key + "\":");
            if (pos == std::string::npos) return 0;
            pos += key.length() + 3;
            while (pos < block.length() && (block[pos] == ' ' || block[pos] == '\t')) pos++;
            try {
                return std::stoll(block.substr(pos));
            } catch (...) {
                return 0;
            }
        };

        auto get_block_str = [&block](const std::string& key) -> std::string {
            size_t pos = block.find("\"" + key + "\":");
            if (pos == std::string::npos) return "";
            pos = block.find('\"', pos + key.length() + 2);
            if (pos == std::string::npos) return "";
            size_t end_s = block.find('\"', pos + 1);
            if (end_s == std::string::npos) return "";
            return block.substr(pos + 1, end_s - pos - 1);
        };

        if (block.find("\"id\":") != std::string::npos) {
            auto chunk = std::make_shared<Chunk>();
            chunk->id = static_cast<uint32_t>(get_block_num("id"));
            chunk->start_offset = get_block_num("start_offset");
            chunk->end_offset = get_block_num("end_offset");
            chunk->downloaded_bytes = get_block_num("downloaded_bytes");
            chunk->status = static_cast<ChunkStatus>(get_block_num("status"));
            chunk->temp_filepath = get_block_str("temp_filepath");
            chunks_.push_back(chunk);
        }

        cur = end + 1;
    }

    return !chunks_.empty();
}

} // namespace opendm
