#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <cstdint>
#include <functional>

namespace opendm {

enum class ChunkStatus {
    PENDING,
    DOWNLOADING,
    COMPLETED,
    FAILED
};

struct Chunk {
    uint32_t id{0};
    int64_t start_offset{0};
    int64_t end_offset{0}; // inclusive
    int64_t downloaded_bytes{0};
    uint64_t current_speed{0}; // bytes per second
    ChunkStatus status{ChunkStatus::PENDING};
    std::string temp_filepath;

    int64_t total_size() const {
        if (end_offset < start_offset) return 0;
        return end_offset - start_offset + 1;
    }

    int64_t remaining_bytes() const {
        int64_t total = total_size();
        return (total > downloaded_bytes) ? (total - downloaded_bytes) : 0;
    }
};

class ChunkManager {
public:
    static constexpr int64_t MIN_CHUNK_SIZE = 102400; // 100 KB threshold (from IDM ChunksList)

    ChunkManager();
    ~ChunkManager() = default;

    // Initialize root chunk covering [0, total_size - 1]
    bool init_first_chunk(int64_t total_size, const std::string& temp_dir, const std::string& file_prefix);

    // Initialize for non-resumable / unknown size download
    bool init_stream_chunk(const std::string& temp_dir, const std::string& file_prefix);

    // IDM algorithm: ChunksList::NeedNewConnection()
    // Checks if max free bytes >= 2 * MIN_CHUNK_SIZE + 3 (204,803 bytes)
    bool need_new_connection(size_t current_connections, size_t max_connections);

    // IDM algorithm: ChunksList::FindMaxFreeChunk()
    int find_max_free_chunk();

    // IDM algorithm: ChunksList::FindChunkWithZipCentralDir()
    // Returns chunk ID covering target_offset and optionally writes remaining bytes to out_remaining_bytes
    int find_chunk_with_zip_central_dir(int64_t target_offset, int64_t* out_remaining_bytes = nullptr);

    // IDM algorithm: ChunksList::CreateNewItem() & Connection::OnChangeBounds()
    // Splits chunk at target_chunk_id, returns newly created chunk ID, or -1 on failure
    int split_chunk(int target_chunk_id, const std::string& temp_dir, const std::string& file_prefix);

    // Update chunk progress
    void update_chunk_progress(int chunk_id, int64_t bytes_received, uint64_t speed_bytes_per_sec);

    // Set chunk status
    void set_chunk_status(int chunk_id, ChunkStatus status);

    // Mark chunk as completed
    void mark_chunk_completed(int chunk_id);

    // Check if all chunks completed
    bool all_chunks_completed() const;

    // Get total downloaded bytes across all chunks
    int64_t get_total_downloaded_bytes() const;

    // Get total file size (or -1 if streaming/unknown)
    int64_t get_total_size() const;

    // Snapshots of chunks for UI
    std::vector<Chunk> get_chunks_snapshot() const;

    // Get specific chunk info
    bool get_chunk(int chunk_id, Chunk& out_chunk) const;

    // IDM algorithm: ChunksList::JoinFile() & AppendFile()
    // Stitches all chunks into the target file in order, then removes temporary chunks
    bool join_file(const std::string& destination_filepath, std::function<void(double)> progress_cb = nullptr);

    // Reset chunk state and clear chunks
    void reset();

    // Clean up temporary .part files from disk
    void cleanup_temp_files();

    // Save and load state for resume
    std::string serialize_state() const;
    bool deserialize_state(const std::string& json_str);

private:
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<Chunk>> chunks_;
    int64_t total_file_size_{-1};
    uint32_t next_chunk_id_{0};
    bool is_streaming_{false};
};

} // namespace opendm
