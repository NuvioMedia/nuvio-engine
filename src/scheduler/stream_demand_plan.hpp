#ifndef NUVIO_ENGINE_STREAM_DEMAND_PLAN_HPP
#define NUVIO_ENGINE_STREAM_DEMAND_PLAN_HPP

#include "nuvio_engine/byte_range.hpp"
#include "nuvio_engine/piece_scheduler.hpp"

#include <cstdint>
#include <vector>

namespace nuvio::scheduler {

struct StreamDemand {
    std::uint64_t id;
    std::uint64_t file_offset;
    std::uint64_t file_size;
    std::uint32_t piece_size;
    http::ByteRange range;
};

struct StreamDemandPlan {
    std::uint64_t focused_demand_id = 0;
    std::vector<PiecePriority> pieces;
    std::vector<std::uint32_t> blocking_deadline_order;
};

[[nodiscard]] StreamDemandPlan build_stream_demand_plan(
    std::vector<StreamDemand> demands,
    std::uint64_t selection_bytes,
    std::uint64_t critical_front_bytes
);

}

#endif
