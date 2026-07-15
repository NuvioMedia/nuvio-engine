#include "test_support.hpp"

#include "scheduler/stream_demand_plan.hpp"

#include <algorithm>
#include <optional>

using nuvio::http::ByteRange;
using nuvio::scheduler::PiecePriority;
using nuvio::scheduler::PriorityClass;
using nuvio::scheduler::StreamDemand;
using nuvio::scheduler::StreamDemandPlan;
using nuvio::scheduler::build_stream_demand_plan;

namespace {

constexpr std::uint64_t mebibyte = 1024ULL * 1024ULL;
constexpr std::uint64_t file_size = 64 * mebibyte;
constexpr std::uint32_t piece_size = static_cast<std::uint32_t>(mebibyte);
constexpr std::uint64_t selection_bytes = 15 * mebibyte;
constexpr std::uint64_t critical_front_bytes = mebibyte;

StreamDemand demand(const std::uint64_t id, const std::uint32_t piece) {
    return {
        id,
        0,
        file_size,
        piece_size,
        ByteRange{
            static_cast<std::uint64_t>(piece) * piece_size,
            (static_cast<std::uint64_t>(piece) + 1) * piece_size - 1,
        },
    };
}

std::optional<PriorityClass> priority_for(
    const StreamDemandPlan& plan,
    const std::uint32_t piece
) {
    const auto found = std::ranges::find(plan.pieces, piece, &PiecePriority::piece);
    return found == plan.pieces.end()
        ? std::nullopt
        : std::optional(found->priority);
}

StreamDemandPlan three_range_plan(std::vector<StreamDemand> demands) {
    return build_stream_demand_plan(
        std::move(demands),
        selection_bytes,
        critical_front_bytes
    );
}

}

NUVIO_TEST("newest stream demand owns deadline focus and lookahead") {
    const auto plan = three_range_plan({demand(10, 0), demand(20, 63), demand(30, 32)});

    NUVIO_EXPECT_EQ(plan.focused_demand_id, std::uint64_t(30));
    NUVIO_EXPECT_EQ(
        plan.blocking_deadline_order,
        (std::vector<std::uint32_t>{32, 63, 0})
    );
    NUVIO_EXPECT_EQ(priority_for(plan, 0), std::optional(PriorityClass::blocking));
    NUVIO_EXPECT_EQ(priority_for(plan, 32), std::optional(PriorityClass::blocking));
    NUVIO_EXPECT_EQ(priority_for(plan, 63), std::optional(PriorityClass::blocking));
    for (std::uint32_t piece = 33; piece <= 44; ++piece) {
        NUVIO_EXPECT_EQ(priority_for(plan, piece), std::optional(PriorityClass::playback));
    }
    NUVIO_EXPECT_TRUE(!priority_for(plan, 1).has_value());
    NUVIO_EXPECT_TRUE(!priority_for(plan, 62).has_value());
    NUVIO_EXPECT_EQ(plan.pieces.size(), std::size_t(15));
}

NUVIO_TEST("stream demand planning is independent of input iteration order") {
    const auto expected = three_range_plan({demand(10, 0), demand(20, 63), demand(30, 32)});
    const auto reversed = three_range_plan({demand(30, 32), demand(20, 63), demand(10, 0)});
    const auto shuffled = three_range_plan({demand(20, 63), demand(10, 0), demand(30, 32)});

    NUVIO_EXPECT_EQ(reversed.focused_demand_id, expected.focused_demand_id);
    NUVIO_EXPECT_EQ(reversed.blocking_deadline_order, expected.blocking_deadline_order);
    NUVIO_EXPECT_EQ(reversed.pieces, expected.pieces);
    NUVIO_EXPECT_EQ(shuffled.focused_demand_id, expected.focused_demand_id);
    NUVIO_EXPECT_EQ(shuffled.blocking_deadline_order, expected.blocking_deadline_order);
    NUVIO_EXPECT_EQ(shuffled.pieces, expected.pieces);
}

NUVIO_TEST("blocking stream pieces survive a saturated selection budget") {
    const auto plan = build_stream_demand_plan(
        {demand(10, 0), demand(20, 63), demand(30, 32)},
        2 * mebibyte,
        critical_front_bytes
    );

    NUVIO_EXPECT_EQ(plan.pieces.size(), std::size_t(3));
    NUVIO_EXPECT_EQ(
        plan.blocking_deadline_order,
        (std::vector<std::uint32_t>{32, 63, 0})
    );
    NUVIO_EXPECT_TRUE(std::ranges::all_of(plan.pieces, [](const PiecePriority& piece) {
        return piece.priority == PriorityClass::blocking;
    }));
}

NUVIO_TEST("overlapping stream blockers are charged and scheduled once") {
    const auto plan = three_range_plan({demand(10, 32), demand(20, 63), demand(30, 32)});

    NUVIO_EXPECT_EQ(plan.focused_demand_id, std::uint64_t(30));
    NUVIO_EXPECT_EQ(
        plan.blocking_deadline_order,
        (std::vector<std::uint32_t>{32, 63})
    );
    NUVIO_EXPECT_EQ(plan.pieces.size(), std::size_t(15));
}

NUVIO_TEST("a blocker inside focused lookahead is not charged twice") {
    const auto plan = three_range_plan({demand(10, 33), demand(20, 32)});

    NUVIO_EXPECT_EQ(plan.pieces.size(), std::size_t(15));
    NUVIO_EXPECT_EQ(priority_for(plan, 32), std::optional(PriorityClass::blocking));
    NUVIO_EXPECT_EQ(priority_for(plan, 33), std::optional(PriorityClass::blocking));
    NUVIO_EXPECT_EQ(priority_for(plan, 46), std::optional(PriorityClass::playback));
    NUVIO_EXPECT_TRUE(!priority_for(plan, 47).has_value());
}

NUVIO_TEST("ending the newest stream demand promotes the next live range") {
    const auto plan = three_range_plan({demand(10, 0), demand(20, 63)});

    NUVIO_EXPECT_EQ(plan.focused_demand_id, std::uint64_t(20));
    NUVIO_EXPECT_EQ(
        plan.blocking_deadline_order,
        (std::vector<std::uint32_t>{63, 0})
    );
    NUVIO_EXPECT_EQ(priority_for(plan, 0), std::optional(PriorityClass::blocking));
    NUVIO_EXPECT_EQ(priority_for(plan, 63), std::optional(PriorityClass::blocking));
}

NUVIO_TEST("lookahead budget charges a full physical torrent piece") {
    constexpr std::uint32_t large_piece = 8 * static_cast<std::uint32_t>(mebibyte);
    const StreamDemand unaligned{
        1,
        0,
        128 * mebibyte,
        large_piece,
        ByteRange{6 * mebibyte, 8 * mebibyte - 1},
    };
    const auto plan = build_stream_demand_plan(
        {unaligned},
        selection_bytes,
        critical_front_bytes
    );

    NUVIO_EXPECT_EQ(plan.pieces.size(), std::size_t(2));
    NUVIO_EXPECT_EQ(priority_for(plan, 0), std::optional(PriorityClass::blocking));
    NUVIO_EXPECT_EQ(priority_for(plan, 1), std::optional(PriorityClass::playback));
    NUVIO_EXPECT_TRUE(!priority_for(plan, 2).has_value());
}

NUVIO_TEST("physical budgeting preserves logical critical coverage") {
    const StreamDemand unaligned{
        1,
        0,
        file_size,
        piece_size,
        ByteRange{mebibyte / 2, mebibyte - 1},
    };
    const auto plan = build_stream_demand_plan(
        {unaligned},
        selection_bytes,
        critical_front_bytes
    );

    NUVIO_EXPECT_EQ(plan.pieces.size(), std::size_t(15));
    NUVIO_EXPECT_EQ(priority_for(plan, 0), std::optional(PriorityClass::blocking));
    NUVIO_EXPECT_EQ(priority_for(plan, 1), std::optional(PriorityClass::critical));
    NUVIO_EXPECT_EQ(priority_for(plan, 2), std::optional(PriorityClass::playback));
}
