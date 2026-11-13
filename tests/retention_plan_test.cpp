#include "test_support.hpp"

#include "scheduler/retention_plan.hpp"

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <unordered_set>
#include <vector>

namespace {

constexpr std::uint64_t mebibyte = 1024 * 1024;

nuvio::scheduler::RetentionWindow middle_window() {
    return {
        0,
        100 * mebibyte,
        static_cast<std::uint32_t>(mebibyte),
        50 * mebibyte,
        4 * mebibyte,
        8 * mebibyte,
        2 * mebibyte,
        20 * mebibyte,
    };
}

std::vector<std::uint32_t> pieces(const std::uint32_t first, const std::uint32_t last) {
    std::vector<std::uint32_t> result(last - first + 1);
    std::iota(result.begin(), result.end(), first);
    return result;
}

bool contains(const std::vector<std::uint32_t>& values, const std::uint32_t value) {
    return std::ranges::find(values, value) != values.end();
}

std::size_t position_of(const std::vector<std::uint32_t>& values, const std::uint32_t value) {
    return static_cast<std::size_t>(std::ranges::find(values, value) - values.begin());
}

}

NUVIO_TEST("retention keeps every piece while the stream is within its budget") {
    const auto retained = pieces(40, 59);
    const auto evictions = nuvio::scheduler::plan_retention_evictions(
        middle_window(),
        retained,
        {},
        256
    );
    NUVIO_EXPECT_TRUE(evictions.empty());
}

NUVIO_TEST("retention evicts the farthest pieces and keeps the playback window and file edges") {
    const auto retained = pieces(0, 99);
    const auto evictions = nuvio::scheduler::plan_retention_evictions(
        middle_window(),
        retained,
        {},
        256
    );
    NUVIO_EXPECT_EQ(evictions.size(), std::size_t(80));
    NUVIO_EXPECT_EQ(evictions.front(), std::uint32_t(2));
    for (std::uint32_t piece = 46; piece <= 58; ++piece) {
        NUVIO_EXPECT_TRUE(!contains(evictions, piece));
    }
    for (const auto edge : {0U, 1U, 98U, 99U}) {
        NUVIO_EXPECT_TRUE(!contains(evictions, edge));
    }
    for (const auto nearest : {43U, 44U, 45U}) {
        NUVIO_EXPECT_TRUE(!contains(evictions, nearest));
    }
    NUVIO_EXPECT_TRUE(position_of(evictions, 3) < position_of(evictions, 97));
}

NUVIO_TEST("retention never evicts protected pieces") {
    const auto retained = pieces(0, 99);
    const std::unordered_set<std::uint32_t> protected_pieces{2, 3, 97};
    const auto evictions = nuvio::scheduler::plan_retention_evictions(
        middle_window(),
        retained,
        protected_pieces,
        256
    );
    NUVIO_EXPECT_EQ(evictions.front(), std::uint32_t(4));
    for (const auto piece : protected_pieces) {
        NUVIO_EXPECT_TRUE(!contains(evictions, piece));
    }
}

NUVIO_TEST("retention evicts pieces outside the streamed file first") {
    auto window = middle_window();
    window.file_offset = 10 * mebibyte;
    window.file_size = 50 * mebibyte;
    window.position = 25 * mebibyte;
    window.budget_bytes = 60 * mebibyte;
    const auto retained = pieces(0, 69);
    const auto evictions = nuvio::scheduler::plan_retention_evictions(
        window,
        retained,
        {},
        256
    );
    NUVIO_EXPECT_EQ(evictions.size(), std::size_t(10));
    for (const auto piece : evictions) {
        NUVIO_EXPECT_TRUE(piece < 10 || piece >= 60);
    }
}

NUVIO_TEST("retention bounds each eviction batch") {
    const auto retained = pieces(0, 99);
    const auto evictions = nuvio::scheduler::plan_retention_evictions(
        middle_window(),
        retained,
        {},
        16
    );
    NUVIO_EXPECT_EQ(evictions.size(), std::size_t(16));
}

NUVIO_TEST("retention clamps a read position past the end of the file") {
    auto window = middle_window();
    window.position = 500 * mebibyte;
    const auto retained = pieces(0, 99);
    const auto evictions = nuvio::scheduler::plan_retention_evictions(
        window,
        retained,
        {},
        256
    );
    NUVIO_EXPECT_EQ(evictions.front(), std::uint32_t(2));
    NUVIO_EXPECT_TRUE(!contains(evictions, 99));
    NUVIO_EXPECT_TRUE(!contains(evictions, 95));
}

NUVIO_TEST("behind-only retention never evicts a piece at or after the reader cursor") {
    // 100 pieces of 1 MiB, cursor in piece 50, everything downloaded and far
    // over budget: only pieces fully behind the cursor may go, oldest first.
    auto window = middle_window();
    window.behind_bytes = 0;
    window.edge_bytes = 0;
    window.budget_bytes = 10 * mebibyte;
    window.behind_only = true;
    const auto retained = pieces(0, 99);
    const auto evictions = nuvio::scheduler::plan_retention_evictions(
        window,
        retained,
        {},
        256
    );
    NUVIO_EXPECT_EQ(evictions.size(), std::size_t(50));
    for (const auto piece : evictions) {
        NUVIO_EXPECT_TRUE(piece < 50);
    }
    NUVIO_EXPECT_EQ(evictions.front(), std::uint32_t(0));
    NUVIO_EXPECT_TRUE(position_of(evictions, 0) < position_of(evictions, 49));
}

NUVIO_TEST("behind-only retention keeps everything when the cursor is at the start") {
    auto window = middle_window();
    window.position = 0;
    window.behind_bytes = 0;
    window.edge_bytes = 0;
    window.budget_bytes = 10 * mebibyte;
    window.behind_only = true;
    const auto retained = pieces(0, 99);
    const auto evictions = nuvio::scheduler::plan_retention_evictions(
        window,
        retained,
        {},
        256
    );
    NUVIO_EXPECT_TRUE(evictions.empty());
}

NUVIO_TEST("behind-only retention still releases pieces outside the streamed file") {
    auto window = middle_window();
    window.file_offset = 10 * mebibyte;
    window.file_size = 50 * mebibyte;
    window.position = 25 * mebibyte;
    window.budget_bytes = 40 * mebibyte;
    window.behind_bytes = 0;
    window.edge_bytes = 0;
    window.behind_only = true;
    const auto retained = pieces(0, 69);
    const auto evictions = nuvio::scheduler::plan_retention_evictions(
        window,
        retained,
        {},
        256
    );
    NUVIO_EXPECT_EQ(evictions.size(), std::size_t(30));
    // Position 25 MiB inside the file is absolute piece 35.
    for (const auto piece : evictions) {
        NUVIO_EXPECT_TRUE(piece < 10 || piece >= 60 || piece < 35);
    }
    NUVIO_EXPECT_TRUE(!contains(evictions, 35));
    NUVIO_EXPECT_TRUE(!contains(evictions, 36));
}
