#ifndef NUVIO_ENGINE_LIBTORRENT_STREAM_BRIDGE_HPP
#define NUVIO_ENGINE_LIBTORRENT_STREAM_BRIDGE_HPP

#include <cstdint>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <libtorrent/fwd.hpp>

#include "cache/verified_piece_cache.hpp"
#include "torrent/protocol_backend.hpp"

namespace nuvio::torrent {

struct PreparedStream {
    std::string stream_id;
    std::string url;
};

struct StoppedStream {
    std::string torrent_id;
    std::string stream_id;
};

struct StreamBridgeStats {
    std::uint32_t active_streams = 0;
    std::uint32_t active_http_requests = 0;
    std::uint32_t active_demands = 0;
    std::uint32_t pending_piece_reads = 0;
    cache::PieceCacheStats cache{};
    std::vector<BackendStats::Stream> streams{};
};

class LibtorrentStreamBridge {
public:
    LibtorrentStreamBridge(
        std::uint16_t requested_port,
        std::uint64_t memory_capacity_bytes,
        std::uint64_t disk_capacity_bytes,
        std::chrono::milliseconds inactivity_timeout
    );
    ~LibtorrentStreamBridge();

    LibtorrentStreamBridge(const LibtorrentStreamBridge&) = delete;
    LibtorrentStreamBridge& operator=(const LibtorrentStreamBridge&) = delete;

    [[nodiscard]] PreparedStream register_stream(
        const lt::torrent_handle& handle,
        const std::string& torrent_id,
        std::uint32_t file_index,
        const TorrentFileInfo& file,
        std::uint32_t stream_flags = 0
    );
    void set_wakeup(std::function<void()> wake);
    void poll();
    [[nodiscard]] bool has_pending_work();
    // Incremented whenever evicted piece ranges were released from payload files.
    [[nodiscard]] std::uint64_t released_range_generation() const;
    [[nodiscard]] std::vector<StoppedStream> pop_expired_streams();
    void handle_read_piece(const lt::read_piece_alert& alert);
    void handle_block_finished(const lt::block_finished_alert& alert);
    void handle_piece_finished(const lt::piece_finished_alert& alert);
    void handle_hash_failed(const lt::hash_failed_alert& alert);
    [[nodiscard]] std::string stop_stream(const std::string& stream_id);
    [[nodiscard]] bool has_stream_for_torrent(const std::string& torrent_id);
    [[nodiscard]] bool has_active_reader(const std::string& torrent_id) const;
    [[nodiscard]] std::vector<std::uint32_t> blocking_pieces(
        const std::string& torrent_id
    ) const;
    void remove_torrent(const std::string& torrent_id);
    void handle_resume_saved(
        const std::string& torrent_id,
        const lt::add_torrent_params& params
    );
    [[nodiscard]] bool has_unpersisted_evictions(const std::string& torrent_id) const;
    [[nodiscard]] std::vector<std::string> pop_resume_save_requests();
    [[nodiscard]] StreamBridgeStats statistics();
    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}

#endif
