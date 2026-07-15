#include "test_support.hpp"

#include "nuvio_engine/piece_scheduler.hpp"
#include "scheduler/demand_window.hpp"

#include <algorithm>

using nuvio::http::ByteRange;
using nuvio::scheduler::PiecePriority;
using nuvio::scheduler::PriorityClass;
using nuvio::scheduler::ScheduleRequest;
using nuvio::scheduler::blocking_demand_range;
using nuvio::scheduler::build_piece_schedule;
using nuvio::scheduler::rolling_lookahead;

namespace {

std::optional<PriorityClass> priority_for(
    const std::vector<PiecePriority>& schedule,
    const std::uint32_t piece
) {
    const auto entry = std::ranges::find(schedule, piece, &PiecePriority::piece);
    return entry == schedule.end() ? std::nullopt : std::optional(entry->priority);
}

}

NUVIO_TEST("blocking demand is limited to the current torrent piece") {
    constexpr std::uint64_t piece_size = 4ULL * 1024ULL * 1024ULL;
    const auto range = blocking_demand_range(
        ByteRange{0, 5ULL * 1024ULL * 1024ULL * 1024ULL},
        0,
        0,
        static_cast<std::uint32_t>(piece_size)
    );
    NUVIO_EXPECT_EQ(range, (ByteRange{0, piece_size - 1}));
}

NUVIO_TEST("blocking demand respects a selected file's piece offset") {
    const auto range = blocking_demand_range(
        ByteRange{0, 8191},
        0,
        512,
        1024
    );
    NUVIO_EXPECT_EQ(range, (ByteRange{0, 511}));

    const auto next = blocking_demand_range(
        ByteRange{0, 8191},
        512,
        512,
        1024
    );
    NUVIO_EXPECT_EQ(next, (ByteRange{512, 1535}));
}

NUVIO_TEST("rolling lookahead keeps the entire foreground selection bounded") {
    constexpr std::uint64_t mebibyte = 1024ULL * 1024ULL;
    const auto large_piece = rolling_lookahead(
        ByteRange{0, 4 * mebibyte - 1},
        15 * mebibyte,
        mebibyte
    );
    NUVIO_EXPECT_EQ(large_piece.critical_bytes, std::uint64_t(0));
    NUVIO_EXPECT_EQ(large_piece.playback_bytes, 11 * mebibyte);

    const auto virtual_piece = rolling_lookahead(
        ByteRange{0, mebibyte / 2 - 1},
        15 * mebibyte,
        mebibyte
    );
    NUVIO_EXPECT_EQ(virtual_piece.critical_bytes, mebibyte / 2);
    NUVIO_EXPECT_EQ(virtual_piece.playback_bytes, 14 * mebibyte);
}

NUVIO_TEST("rolling lookahead is empty when blocking demand fills the selection") {
    const auto lookahead = rolling_lookahead(ByteRange{0, 4095}, 4096, 1024);
    NUVIO_EXPECT_EQ(lookahead.critical_bytes, std::uint64_t(0));
    NUVIO_EXPECT_EQ(lookahead.playback_bytes, std::uint64_t(0));
}

NUVIO_TEST("HTTP demand maps through the file offset to torrent pieces") {
    const auto schedule = build_piece_schedule({
        .file_offset = 512,
        .file_size = 4096,
        .piece_size = 1024,
        .demand = ByteRange{600, 1600},
        .critical_bytes = 0,
        .playback_bytes = 0,
        .readahead_bytes = 0,
        .metadata_tail_bytes = 0,
    });
    NUVIO_EXPECT_EQ(schedule.size(), std::size_t(2));
    NUVIO_EXPECT_EQ(priority_for(schedule, 1), std::optional(PriorityClass::blocking));
    NUVIO_EXPECT_EQ(priority_for(schedule, 2), std::optional(PriorityClass::blocking));
}

NUVIO_TEST("overlapping windows keep the highest urgency") {
    const auto schedule = build_piece_schedule({
        .file_offset = 0,
        .file_size = 8192,
        .piece_size = 1024,
        .demand = ByteRange{0, 1200},
        .critical_bytes = 1000,
        .playback_bytes = 2000,
        .readahead_bytes = 1000,
        .metadata_tail_bytes = 1500,
    });
    NUVIO_EXPECT_EQ(priority_for(schedule, 0), std::optional(PriorityClass::blocking));
    NUVIO_EXPECT_EQ(priority_for(schedule, 1), std::optional(PriorityClass::blocking));
    NUVIO_EXPECT_EQ(priority_for(schedule, 2), std::optional(PriorityClass::critical));
    NUVIO_EXPECT_EQ(priority_for(schedule, 3), std::optional(PriorityClass::playback));
    NUVIO_EXPECT_EQ(priority_for(schedule, 7), std::optional(PriorityClass::metadata_tail));
}

NUVIO_TEST("metadata tail overlapping playback is not demoted") {
    const auto schedule = build_piece_schedule({
        .file_offset = 0,
        .file_size = 4096,
        .piece_size = 1024,
        .demand = ByteRange{2048, 2500},
        .critical_bytes = 0,
        .playback_bytes = 2048,
        .readahead_bytes = 0,
        .metadata_tail_bytes = 2048,
    });
    NUVIO_EXPECT_EQ(priority_for(schedule, 2), std::optional(PriorityClass::blocking));
    NUVIO_EXPECT_EQ(priority_for(schedule, 3), std::optional(PriorityClass::playback));
}

NUVIO_TEST("schedule ordering is urgency first") {
    const auto schedule = build_piece_schedule({
        .file_offset = 0,
        .file_size = 8192,
        .piece_size = 1024,
        .demand = ByteRange{4096, 4500},
        .critical_bytes = 1024,
        .playback_bytes = 0,
        .readahead_bytes = 0,
        .metadata_tail_bytes = 1024,
    });
    NUVIO_EXPECT_EQ(schedule.front().priority, PriorityClass::blocking);
    NUVIO_EXPECT_EQ(schedule.back().priority, PriorityClass::metadata_tail);
}
