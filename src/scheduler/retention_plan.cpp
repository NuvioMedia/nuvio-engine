#include "scheduler/retention_plan.hpp"

#include <algorithm>
#include <limits>

namespace nuvio::scheduler {
namespace {

struct Candidate {
    std::uint32_t piece = 0;
    std::uint64_t distance = 0;
    bool behind = false;
};

std::uint64_t saturating_multiply(const std::uint64_t left, const std::uint64_t right) {
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return left * right;
}

}

std::vector<std::uint32_t> plan_retention_evictions(
    const RetentionWindow& window,
    const std::span<const std::uint32_t> retained_pieces,
    const std::unordered_set<std::uint32_t>& protected_pieces,
    const std::size_t maximum_evictions
) {
    if (window.piece_size == 0 || window.file_size == 0 || maximum_evictions == 0 ||
        window.file_size > std::numeric_limits<std::uint64_t>::max() - window.file_offset) {
        return {};
    }
    const std::uint64_t piece_bytes = window.piece_size;
    const auto retained_bytes = saturating_multiply(retained_pieces.size(), piece_bytes);
    if (retained_bytes <= window.budget_bytes) {
        return {};
    }
    const auto needed = (retained_bytes - window.budget_bytes + piece_bytes - 1) / piece_bytes;

    const auto file_start = window.file_offset;
    const auto file_last = window.file_offset + window.file_size - 1;
    const auto first_file_piece = file_start / piece_bytes;
    const auto last_file_piece = file_last / piece_bytes;
    const auto position = file_start + std::min(window.position, window.file_size - 1);
    const auto position_piece = position / piece_bytes;
    const auto keep_first = (position - std::min(window.behind_bytes, position - file_start)) /
        piece_bytes;
    const auto keep_last = (position + std::min(window.ahead_bytes, file_last - position)) /
        piece_bytes;
    const auto edge = std::min(window.edge_bytes, window.file_size);
    const auto head_last = edge == 0 ? first_file_piece : (file_start + edge - 1) / piece_bytes;
    const auto tail_first = edge == 0 ? last_file_piece : (file_last + 1 - edge) / piece_bytes;

    std::vector<Candidate> candidates;
    candidates.reserve(retained_pieces.size());
    for (const auto piece : retained_pieces) {
        if (protected_pieces.contains(piece)) {
            continue;
        }
        if (piece >= keep_first && piece <= keep_last) {
            continue;
        }
        if (window.behind_only && piece >= position_piece && piece <= last_file_piece) {
            continue;
        }
        if (edge > 0 && piece >= first_file_piece &&
            (piece <= head_last || (piece >= tail_first && piece <= last_file_piece))) {
            continue;
        }
        const auto outside_file = piece < first_file_piece || piece > last_file_piece;
        const auto distance = outside_file
            ? std::numeric_limits<std::uint64_t>::max()
            : piece > position_piece ? piece - position_piece : position_piece - piece;
        candidates.push_back({piece, distance, piece < position_piece});
    }
    std::ranges::sort(candidates, [](const Candidate& left, const Candidate& right) {
        if (left.distance != right.distance) {
            return left.distance > right.distance;
        }
        if (left.behind != right.behind) {
            return left.behind;
        }
        return left.piece < right.piece;
    });

    const auto count = std::min({
        static_cast<std::size_t>(std::min<std::uint64_t>(
            needed,
            std::numeric_limits<std::size_t>::max()
        )),
        maximum_evictions,
        candidates.size(),
    });
    std::vector<std::uint32_t> result;
    result.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        result.push_back(candidates[index].piece);
    }
    return result;
}

}
