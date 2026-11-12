#include "scheduler/stream_throttle.hpp"

#include <algorithm>

namespace nuvio::scheduler {

std::vector<TorrentBudget> plan_torrent_budgets(
    const std::span<const TorrentStreamLoad> torrents,
    const ThrottlePolicy& policy
) {
    const auto playback_active = std::ranges::any_of(
        torrents,
        [](const TorrentStreamLoad& torrent) {
            return torrent.streaming && torrent.playback_reader;
        }
    );
    const auto throttled = [&](const TorrentStreamLoad& torrent) {
        return playback_active && torrent.streaming && torrent.bulk_only &&
            !torrent.playback_reader;
    };

    int streaming = 0;
    int throttled_count = 0;
    for (const auto& torrent : torrents) {
        if (!torrent.streaming) {
            continue;
        }
        ++streaming;
        if (throttled(torrent)) {
            ++throttled_count;
        }
    }
    const auto unthrottled = streaming - throttled_count;
    const auto remaining_connections = std::max(
        policy.connection_limit - throttled_count * policy.minimum_connections,
        0
    );
    const auto fair_cap = unthrottled <= 1
        ? -1
        : std::max(policy.minimum_connections, remaining_connections / unthrottled);

    std::vector<TorrentBudget> result;
    result.reserve(torrents.size());
    for (const auto& torrent : torrents) {
        if (throttled(torrent)) {
            result.push_back({policy.minimum_connections, policy.throttled_download_limit});
        } else {
            result.push_back({fair_cap, -1});
        }
    }
    return result;
}

}
