#ifndef NUVIO_ENGINE_STREAM_THROTTLE_HPP
#define NUVIO_ENGINE_STREAM_THROTTLE_HPP

#include <cstdint>
#include <span>
#include <vector>

namespace nuvio::scheduler {

// What the engine knows about one torrent handle.
struct TorrentStreamLoad {
    // The torrent has at least one live stream (playback or bulk).
    bool streaming = false;
    // A non-bulk stream of this torrent is being actively read.
    bool playback_reader = false;
    // Every live stream of this torrent is a bulk (download) stream.
    bool bulk_only = false;
};

struct TorrentBudget {
    // Per-torrent connection cap; -1 removes the cap.
    int max_connections = -1;
    // Per-torrent download limit in bytes per second; -1 removes the limit.
    int download_limit = -1;
};

struct ThrottlePolicy {
    // Session-wide connection limit shared by all torrents.
    int connection_limit = 0;
    // Smallest per-torrent cap handed out.
    int minimum_connections = 20;
    // Download limit of a throttled bulk torrent.
    int throttled_download_limit = 256 * 1024;
};

// Bulk-only torrents are throttled (few connections, small download limit)
// while any torrent has an actively read playback stream, unless they also
// serve a playback reader themselves. Without that, the connection limit is
// split fairly between streaming torrents and nothing is rate limited. With a
// single unthrottled streaming torrent its per-torrent cap is removed.
[[nodiscard]] std::vector<TorrentBudget> plan_torrent_budgets(
    std::span<const TorrentStreamLoad> torrents,
    const ThrottlePolicy& policy
);

}

#endif
