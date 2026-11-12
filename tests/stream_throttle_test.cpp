#include "test_support.hpp"

#include "scheduler/stream_throttle.hpp"

#include <vector>

namespace {

using nuvio::scheduler::ThrottlePolicy;
using nuvio::scheduler::TorrentStreamLoad;

constexpr ThrottlePolicy policy{120, 20, 256 * 1024};

constexpr TorrentStreamLoad idle{false, false, false};
constexpr TorrentStreamLoad playback{true, true, false};
constexpr TorrentStreamLoad bulk{true, false, true};
constexpr TorrentStreamLoad paused_playback{true, false, false};

}

NUVIO_TEST("a lone stream keeps the whole connection limit and no rate limit") {
    const std::vector<TorrentStreamLoad> torrents{playback, idle};
    const auto budgets = nuvio::scheduler::plan_torrent_budgets(torrents, policy);
    NUVIO_EXPECT_EQ(budgets.size(), std::size_t(2));
    NUVIO_EXPECT_EQ(budgets[0].max_connections, -1);
    NUVIO_EXPECT_EQ(budgets[0].download_limit, -1);
    NUVIO_EXPECT_EQ(budgets[1].max_connections, -1);
}

NUVIO_TEST("a bulk torrent is throttled while another torrent plays back") {
    const std::vector<TorrentStreamLoad> torrents{bulk, playback};
    const auto budgets = nuvio::scheduler::plan_torrent_budgets(torrents, policy);
    NUVIO_EXPECT_EQ(budgets[0].max_connections, 20);
    NUVIO_EXPECT_EQ(budgets[0].download_limit, 256 * 1024);
    NUVIO_EXPECT_EQ(budgets[1].max_connections, -1);
    NUVIO_EXPECT_EQ(budgets[1].download_limit, -1);
}

NUVIO_TEST("bulk torrents run unthrottled when no playback stream is being read") {
    const std::vector<TorrentStreamLoad> torrents{bulk, bulk, paused_playback};
    const auto budgets = nuvio::scheduler::plan_torrent_budgets(torrents, policy);
    for (const auto& budget : budgets) {
        NUVIO_EXPECT_EQ(budget.download_limit, -1);
        NUVIO_EXPECT_EQ(budget.max_connections, 40);
    }
}

NUVIO_TEST("a torrent serving a playback reader is never throttled") {
    const TorrentStreamLoad mixed{true, true, false};
    const std::vector<TorrentStreamLoad> torrents{bulk, mixed};
    const auto budgets = nuvio::scheduler::plan_torrent_budgets(torrents, policy);
    NUVIO_EXPECT_EQ(budgets[1].download_limit, -1);
    NUVIO_EXPECT_EQ(budgets[1].max_connections, -1);
    NUVIO_EXPECT_EQ(budgets[0].download_limit, 256 * 1024);
}

NUVIO_TEST("throttled torrents leave the remaining connections to playback torrents") {
    const std::vector<TorrentStreamLoad> torrents{bulk, bulk, playback, playback};
    const auto budgets = nuvio::scheduler::plan_torrent_budgets(torrents, policy);
    NUVIO_EXPECT_EQ(budgets[0].max_connections, 20);
    NUVIO_EXPECT_EQ(budgets[1].max_connections, 20);
    // (120 - 2 * 20) / 2 playback torrents.
    NUVIO_EXPECT_EQ(budgets[2].max_connections, 40);
    NUVIO_EXPECT_EQ(budgets[3].max_connections, 40);
}

NUVIO_TEST("a paused playback stream does not throttle bulk downloads") {
    const std::vector<TorrentStreamLoad> torrents{bulk, paused_playback};
    const auto budgets = nuvio::scheduler::plan_torrent_budgets(torrents, policy);
    NUVIO_EXPECT_EQ(budgets[0].download_limit, -1);
    NUVIO_EXPECT_EQ(budgets[0].max_connections, 60);
}

NUVIO_TEST("per-torrent caps never drop below the minimum") {
    const std::vector<TorrentStreamLoad> torrents{
        playback, playback, playback, playback, playback, playback, playback,
    };
    const auto budgets = nuvio::scheduler::plan_torrent_budgets(torrents, policy);
    for (const auto& budget : budgets) {
        NUVIO_EXPECT_EQ(budget.max_connections, 20);
    }
}
