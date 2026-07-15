#include "scheduler/stream_demand_plan.hpp"

#include "scheduler/demand_window.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>

namespace nuvio::scheduler {
namespace {

std::vector<PiecePriority> schedule_for(
    const StreamDemand& demand,
    const std::uint64_t critical_bytes,
    const std::uint64_t playback_bytes
) {
    return build_piece_schedule({
        demand.file_offset,
        demand.file_size,
        demand.piece_size,
        demand.range,
        critical_bytes,
        playback_bytes,
        0,
        0,
    });
}

void merge_schedule(
    std::map<std::uint32_t, PriorityClass>& combined,
    const std::vector<PiecePriority>& schedule
) {
    for (const auto& priority : schedule) {
        const auto existing = combined.find(priority.piece);
        if (existing == combined.end() || priority.priority < existing->second) {
            combined[priority.piece] = priority.priority;
        }
    }
}

std::uint64_t physical_piece_budget(
    const std::uint64_t selection_bytes,
    const std::uint32_t piece_size
) {
    if (selection_bytes == 0 || piece_size == 0) {
        return 0;
    }
    return 1 + (selection_bytes - 1) / piece_size;
}

}

StreamDemandPlan build_stream_demand_plan(
    std::vector<StreamDemand> demands,
    const std::uint64_t selection_bytes,
    const std::uint64_t critical_front_bytes
) {
    StreamDemandPlan result;
    if (demands.empty()) {
        return result;
    }

    std::ranges::sort(demands, [](const StreamDemand& left, const StreamDemand& right) {
        return left.id < right.id;
    });

    std::map<std::uint32_t, PriorityClass> combined;
    std::map<std::uint64_t, std::vector<PiecePriority>> blocking_schedules;
    for (const auto& demand : demands) {
        auto schedule = schedule_for(demand, 0, 0);
        merge_schedule(combined, schedule);
        blocking_schedules.insert_or_assign(demand.id, std::move(schedule));
    }

    std::set<std::uint32_t> ordered_blockers;
    for (auto demand = demands.rbegin(); demand != demands.rend(); ++demand) {
        const auto schedule = blocking_schedules.find(demand->id);
        if (schedule == blocking_schedules.end()) {
            continue;
        }
        for (const auto& priority : schedule->second) {
            if (priority.priority == PriorityClass::blocking &&
                ordered_blockers.insert(priority.piece).second) {
                result.blocking_deadline_order.push_back(priority.piece);
            }
        }
    }

    const auto& focused = demands.back();
    result.focused_demand_id = focused.id;
    const auto focused_schedule = blocking_schedules.find(focused.id);
    std::set<std::uint32_t> focused_blockers;
    if (focused_schedule != blocking_schedules.end()) {
        for (const auto& priority : focused_schedule->second) {
            if (priority.priority == PriorityClass::blocking) {
                focused_blockers.insert(priority.piece);
            }
        }
    }

    const auto budget_pieces = physical_piece_budget(selection_bytes, focused.piece_size);
    const auto target_pieces = std::max(
        budget_pieces,
        static_cast<std::uint64_t>(combined.size())
    );
    std::set<std::uint32_t> critical_pieces;
    const auto lookahead = rolling_lookahead(
        focused.range,
        std::numeric_limits<std::uint64_t>::max(),
        critical_front_bytes
    );
    for (const auto& priority : schedule_for(focused, lookahead.critical_bytes, 0)) {
        if (priority.priority == PriorityClass::critical) {
            critical_pieces.insert(priority.piece);
        }
    }
    if (!focused_blockers.empty() && focused.file_size > 0 &&
        focused.piece_size > 0 &&
        focused.file_offset <= std::numeric_limits<std::uint64_t>::max() -
            (focused.file_size - 1)) {
        const auto last_file_piece =
            (focused.file_offset + focused.file_size - 1) / focused.piece_size;
        const auto focused_piece = static_cast<std::uint64_t>(*focused_blockers.rbegin());
        for (auto candidate = focused_piece + 1;
             candidate <= last_file_piece &&
                 static_cast<std::uint64_t>(combined.size()) < target_pieces;
             ++candidate) {
            if (candidate > std::numeric_limits<std::uint32_t>::max()) {
                break;
            }
            const auto piece = static_cast<std::uint32_t>(candidate);
            if (combined.contains(piece)) {
                continue;
            }
            combined.emplace(
                piece,
                critical_pieces.contains(piece)
                    ? PriorityClass::critical
                    : PriorityClass::playback
            );
        }
    }

    result.pieces.reserve(combined.size());
    for (const auto& [piece, priority] : combined) {
        result.pieces.push_back({piece, priority});
    }
    std::ranges::stable_sort(result.pieces, {}, &PiecePriority::priority);
    return result;
}

}
