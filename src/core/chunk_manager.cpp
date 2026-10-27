#include "chunk_manager.hpp"
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <sstream>
#include <iostream>

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

    return (max_free >= 2 * MIN_CHUNK_SIZE);
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

    if (remaining < 2 * MIN_CHUNK_SIZE) {
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
    std::lock_guard<std::mutex> lock(mutex_);
    if (chunks_.empty()) {
        return false;
    }

    // Sort chunks by start_offset
    std::vector<std::shared_ptr<Chunk>> sorted_chunks = chunks_;
    std::sort(sorted_chunks.begin(), sorted_chunks.end(),
        [](const std::shared_ptr<Chunk>& a, const std::shared_ptr<Chunk>& b) {
            return a->start_offset < b->start_offset;
        });

    // Ensure parent directory exists
    std::filesystem::path dest_p(destination_filepath);
    if (dest_p.has_parent_path()) {
        std::filesystem::create_directories(dest_p.parent_path());
    }

    std::ofstream dest_file(destination_filepath, std::ios::binary | std::ios::trunc);
    if (!dest_file.is_open()) {
        return false;
    }

    int64_t total_written = 0;
    constexpr size_t BUFFER_SIZE = 64 * 1024; // 64 KB buffer as in IDM AppendFile
    std::vector<char> buffer(BUFFER_SIZE);

    for (size_t i = 0; i < sorted_chunks.size(); ++i) {
        const auto& chunk = sorted_chunks[i];
        std::ifstream part_file(chunk->temp_filepath, std::ios::binary);
        if (!part_file.is_open()) {
            dest_file.close();
            return false;
        }

        while (part_file.good()) {
            part_file.read(buffer.data(), buffer.size());
            std::streamsize bytes_read = part_file.gcount();
            if (bytes_read > 0) {
                dest_file.write(buffer.data(), bytes_read);
                total_written += bytes_read;
                if (progress_cb && total_file_size_ > 0) {
                    double progress = static_cast<double>(total_written) / total_file_size_;
                    progress_cb(std::min(1.0, progress));
                }
            }
        }
        part_file.close();

        // Delete temporary part file after stitching
        std::error_code ec;
        std::filesystem::remove(chunk->temp_filepath, ec);
    }

    dest_file.flush();
    dest_file.close();

    if (progress_cb) {
        progress_cb(1.0);
    }

    return true;
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
