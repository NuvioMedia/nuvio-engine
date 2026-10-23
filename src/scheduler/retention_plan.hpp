#ifndef NUVIO_ENGINE_RETENTION_PLAN_HPP
#define NUVIO_ENGINE_RETENTION_PLAN_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_set>
#include <vector>

namespace nuvio::scheduler {

struct RetentionWindow {
    std::uint64_t file_offset = 0;
    std::uint64_t file_size = 0;
    std::uint32_t piece_size = 0;
    std::uint64_t position = 0;
    std::uint64_t behind_bytes = 0;
    std::uint64_t ahead_bytes = 0;
    std::uint64_t edge_bytes = 0;
    std::uint64_t budget_bytes = 0;
};

[[nodiscard]] std::vector<std::uint32_t> plan_retention_evictions(
    const RetentionWindow& window,
    std::span<const std::uint32_t> retained_pieces,
    const std::unordered_set<std::uint32_t>& protected_pieces,
    std::size_t maximum_evictions
);

}

#endif
