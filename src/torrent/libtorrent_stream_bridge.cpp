#include "torrent/libtorrent_stream_bridge.hpp"

#include "cache/verified_piece_cache.hpp"
#include "http/loopback_server.hpp"
#include "nuvio_engine/piece_scheduler.hpp"
#include "scheduler/demand_window.hpp"
#include "scheduler/stream_demand_plan.hpp"
#include "security/random_bytes.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <libtorrent/alert_types.hpp>
#include <libtorrent/download_priority.hpp>
#include <libtorrent/hex.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_info.hpp>

namespace nuvio::torrent {
namespace {

constexpr std::size_t maximum_http_commands = 4096;
constexpr std::size_t stream_worker_count = 8;
constexpr std::chrono::seconds piece_wait_timeout{60};
constexpr std::uint64_t rolling_selection_limit = 15 * 1024 * 1024;
constexpr std::uint64_t critical_front_limit = 1024 * 1024;

std::string torrent_id(const lt::torrent_handle& handle) {
    const auto hashes = handle.info_hashes();
    if (hashes.has_v1()) {
        return lt::aux::to_hex(hashes.v1);
    }
    if (hashes.has_v2()) {
        return lt::aux::to_hex(hashes.v2);
    }
    return {};
}

std::string content_type_for_path(const std::string& path) {
    std::string normalized = path;
    std::ranges::transform(normalized, normalized.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    if (normalized.ends_with(".mp4") || normalized.ends_with(".m4v")) {
        return "video/mp4";
    }
    if (normalized.ends_with(".mkv")) {
        return "video/x-matroska";
    }
    if (normalized.ends_with(".webm")) {
        return "video/webm";
    }
    if (normalized.ends_with(".avi")) {
        return "video/x-msvideo";
    }
    if (normalized.ends_with(".ts") || normalized.ends_with(".mpeg") ||
        normalized.ends_with(".mpg")) {
        return "video/mp2t";
    }
    if (normalized.ends_with(".mov")) {
        return "video/quicktime";
    }
    if (normalized.ends_with(".mp3")) {
        return "audio/mpeg";
    }
    if (normalized.ends_with(".flac")) {
        return "audio/flac";
    }
    return "application/octet-stream";
}

lt::download_priority_t download_priority_for(
    const scheduler::PriorityClass priority
) {
    switch (priority) {
    case scheduler::PriorityClass::blocking:
        return lt::top_priority;
    case scheduler::PriorityClass::critical:
        return lt::download_priority_t{6};
    case scheduler::PriorityClass::playback:
        return lt::default_priority;
    case scheduler::PriorityClass::metadata_tail:
        return lt::download_priority_t{3};
    case scheduler::PriorityClass::readahead:
        return lt::low_priority;
    }
    return lt::dont_download;
}

int blocking_deadline_for(const std::size_t ordinal) {
    const auto increment = std::min<std::size_t>(ordinal * 5, 400);
    return static_cast<int>(increment);
}

}

struct LibtorrentStreamBridge::Impl {
    struct StreamCounters {
        std::atomic_uint64_t delivered_bytes = 0;
    };

    struct StreamActivity {
        using Clock = std::chrono::steady_clock;

        explicit StreamActivity(const Clock::time_point now)
            : last_activity(now) {
        }

        void acquire_http_request() noexcept {
            std::lock_guard lock(mutex);
            ++active_http_requests;
        }

        void release_http_request() noexcept {
            std::lock_guard lock(mutex);
            if (active_http_requests == 0) {
                return;
            }
            --active_http_requests;
            if (active_http_requests == 0) {
                last_activity = Clock::now();
            }
        }

        [[nodiscard]] bool is_inactive(
            const Clock::time_point now,
            const std::chrono::milliseconds timeout
        ) const noexcept {
            std::lock_guard lock(mutex);
            return active_http_requests == 0 && now - last_activity >= timeout;
        }

        mutable std::mutex mutex;
        std::uint32_t active_http_requests = 0;
        Clock::time_point last_activity;
    };

    struct HttpRequestLease {
        explicit HttpRequestLease(std::shared_ptr<StreamActivity> activity)
            : activity_(std::move(activity)) {
        }

        ~HttpRequestLease() {
            activity_->release_http_request();
        }

        HttpRequestLease(const HttpRequestLease&) = delete;
        HttpRequestLease& operator=(const HttpRequestLease&) = delete;

    private:
        std::shared_ptr<StreamActivity> activity_;
    };

    static void add_delivered_bytes(
        const std::shared_ptr<StreamCounters>& counters,
        const std::uint64_t bytes
    ) {
        auto current = counters->delivered_bytes.load(std::memory_order_relaxed);
        while (true) {
            const auto next = bytes > std::numeric_limits<std::uint64_t>::max() - current
                ? std::numeric_limits<std::uint64_t>::max()
                : current + bytes;
            if (counters->delivered_bytes.compare_exchange_weak(
                    current,
                    next,
                    std::memory_order_relaxed
                )) {
                return;
            }
        }
    }

    struct StreamRoute {
        lt::torrent_handle handle;
        std::string torrent_id;
        std::string stream_id;
        std::string target;
        std::string content_type;
        std::uint32_t file_index = 0;
        std::uint32_t piece_size = 0;
        std::uint64_t file_offset = 0;
        std::uint64_t file_size = 0;
        std::shared_ptr<StreamActivity> activity;
        std::shared_ptr<StreamCounters> counters = std::make_shared<StreamCounters>();
    };

    using PieceKey = cache::PieceCacheKey;
    using PieceKeyHash = cache::PieceCacheKeyHash;

    struct PieceWaiter {
        std::mutex mutex;
        std::condition_variable ready;
        std::shared_ptr<std::vector<char>> data;
        std::string error;
        bool done = false;
        bool cancelled = false;
        std::uint64_t demand_id = 0;
    };

    struct DemandState {
        std::atomic_bool cancelled = false;
    };

    enum class CommandType {
        begin_demand,
        end_demand,
        read_piece,
    };

    struct HttpCommand {
        CommandType type;
        std::uint64_t demand_id = 0;
        StreamRoute route;
        http::ByteRange range{};
        std::uint32_t piece = 0;
        std::shared_ptr<PieceWaiter> waiter;
        std::shared_ptr<DemandState> demand_state;
    };

    struct ActiveDemand {
        std::uint64_t id = 0;
        StreamRoute route;
        http::ByteRange range{};
        std::shared_ptr<DemandState> state;
    };

    struct CurrentSchedule {
        lt::torrent_handle handle;
        std::map<std::uint32_t, scheduler::PriorityClass> priorities;
        std::vector<std::uint32_t> blocking_deadline_order;
        std::uint64_t focused_demand_id = 0;
        std::uint64_t revision = 0;
    };

    struct DemandGuard {
        Impl* owner;
        std::uint64_t demand_id;

        ~DemandGuard() {
            try {
                owner->enqueue_end(demand_id);
            } catch (...) {
            }
        }
    };

    static http::ByteRange bounded_demand_range(
        const StreamRoute& route,
        const http::ByteRange response_range,
        const std::uint64_t position
    ) {
        return scheduler::blocking_demand_range(
            response_range,
            position,
            route.file_offset,
            route.piece_size
        );
    }

    Impl(
        const std::uint16_t requested_port,
        const std::uint64_t memory_capacity_bytes,
        const std::chrono::milliseconds inactivity
    )
        : rolling_selection_bytes(std::min(
              memory_capacity_bytes,
              rolling_selection_limit
          )),
          inactivity_timeout(inactivity),
          piece_cache(memory_capacity_bytes),
          server(std::make_unique<http::LoopbackServer>(
              requested_port,
              [this](
                  const http::HttpRequest& request,
                  const http::LoopbackServer::Writer& writer,
                  const std::stop_token token
              ) {
                  handle_http(request, writer, token);
              },
              stream_worker_count
          )) {
    }

    ~Impl() {
        try {
            shutdown();
        } catch (...) {
        }
    }

    PreparedStream register_stream(
        const lt::torrent_handle& handle,
        const std::string& id,
        const std::uint32_t file_index,
        const TorrentFileInfo& file
    ) {
        if (shutting_down.load()) {
            throw std::runtime_error("stream bridge is shutting down");
        }
        const auto metadata = handle.torrent_file();
        if (!metadata || metadata->piece_length() <= 0) {
            throw std::runtime_error("torrent piece metadata is unavailable");
        }
        StreamRoute route{
            handle,
            id,
            security::random_hex_token(32),
            {},
            content_type_for_path(file.path),
            file_index,
            static_cast<std::uint32_t>(metadata->piece_length()),
            file.offset,
            file.size,
            std::make_shared<StreamActivity>(std::chrono::steady_clock::now()),
        };
        route.target = "/stream/" + route.stream_id;
        std::vector<std::string> previous_streams;
        {
            std::lock_guard lock(routes_mutex);
            for (const auto& [target, existing] : routes) {
                static_cast<void>(target);
                if (existing.torrent_id == id) {
                    previous_streams.push_back(existing.stream_id);
                }
            }
        }
        for (const auto& stream_id : previous_streams) {
            static_cast<void>(stop_stream(stream_id));
        }
        {
            std::lock_guard lock(routes_mutex);
            routes.insert_or_assign(route.target, route);
        }
        next_progress_sample = {};
        last_ready_pieces.erase(id);
        revoked_torrents.erase(id);
        return {
            route.stream_id,
            "http://127.0.0.1:" + std::to_string(server->port()) + route.target,
        };
    }

    void handle_http(
        const http::HttpRequest& request,
        const http::LoopbackServer::Writer& writer,
        const std::stop_token token
    ) {
        StreamRoute route;
        {
            std::lock_guard lock(routes_mutex);
            const auto found = routes.find(request.target);
            if (found == routes.end()) {
                const auto response = http::build_error_response(http::ResponseStatus::not_found);
                static_cast<void>(writer(response));
                return;
            }
            route = found->second;
            route.activity->acquire_http_request();
        }
        const HttpRequestLease request_lease(route.activity);
        const auto response = http::build_stream_response(
            request,
            route.file_size,
            route.content_type
        );
        if (!response.include_body || !response.body_range.has_value()) {
            static_cast<void>(writer(response.headers));
            return;
        }

        const auto demand_id = next_demand_id.fetch_add(1);
        auto demand_state = std::make_shared<DemandState>();
        if (!enqueue({
                CommandType::begin_demand,
                demand_id,
                route,
                bounded_demand_range(
                    route,
                    *response.body_range,
                    response.body_range->start
                ),
                0,
                {},
                demand_state,
            })) {
            const auto unavailable = http::build_error_response(
                http::ResponseStatus::service_unavailable
            );
            static_cast<void>(writer(unavailable));
            return;
        }
        DemandGuard guard{this, demand_id};
        if (!writer(response.headers)) {
            return;
        }
        stream_body(
            demand_id,
            demand_state,
            route,
            *response.body_range,
            writer,
            token
        );
    }

    void stream_body(
        const std::uint64_t demand_id,
        const std::shared_ptr<DemandState>& demand_state,
        const StreamRoute& route,
        const http::ByteRange range,
        const http::LoopbackServer::Writer& writer,
        const std::stop_token token
    ) {
        if (range.end > std::numeric_limits<std::uint64_t>::max() - route.file_offset) {
            return;
        }
        const auto absolute_start = route.file_offset + range.start;
        const auto absolute_end = route.file_offset + range.end;
        if (absolute_end == std::numeric_limits<std::uint64_t>::max()) {
            return;
        }
        const auto absolute_end_exclusive = absolute_end + 1;
        const auto first_piece = absolute_start / route.piece_size;
        const auto last_piece = absolute_end / route.piece_size;
        if (last_piece > std::numeric_limits<std::uint32_t>::max()) {
            return;
        }
        for (auto piece = first_piece;
             piece <= last_piece && !token.stop_requested() &&
                 !demand_state->cancelled.load();
             ++piece) {
            const auto piece_start = piece * route.piece_size;
            const auto current_absolute = std::max(absolute_start, piece_start);
            if (current_absolute < route.file_offset) {
                return;
            }
            const auto current_position = current_absolute - route.file_offset;
            const auto data = wait_for_piece(
                demand_id,
                demand_state,
                route,
                static_cast<std::uint32_t>(piece),
                bounded_demand_range(route, range, current_position),
                token
            );
            if (!data.has_value()) {
                return;
            }
            const auto slice_start = std::max(absolute_start, piece_start) - piece_start;
            if ((*data)->size() >
                std::numeric_limits<std::uint64_t>::max() - piece_start) {
                return;
            }
            const auto available_end = piece_start + (*data)->size();
            const auto slice_end = std::min(absolute_end_exclusive, available_end) - piece_start;
            if (slice_start >= slice_end || slice_end > (*data)->size()) {
                return;
            }
            const auto bytes = std::span<const char>(**data).subspan(
                static_cast<std::size_t>(slice_start),
                static_cast<std::size_t>(slice_end - slice_start)
            );
            if (!writer(bytes)) {
                return;
            }
            add_delivered_bytes(route.counters, static_cast<std::uint64_t>(bytes.size()));
        }
    }

    std::optional<std::shared_ptr<std::vector<char>>> wait_for_piece(
        const std::uint64_t demand_id,
        const std::shared_ptr<DemandState>& demand_state,
        const StreamRoute& route,
        const std::uint32_t piece,
        const http::ByteRange demand_range,
        const std::stop_token token
    ) {
        auto waiter = std::make_shared<PieceWaiter>();
        waiter->demand_id = demand_id;
        if (!enqueue({
                CommandType::read_piece,
                demand_id,
                route,
                demand_range,
                piece,
                waiter,
                demand_state,
            })) {
            return std::nullopt;
        }
        const auto deadline = std::chrono::steady_clock::now() + piece_wait_timeout;
        std::stop_callback stop_wait(token, [waiter] {
            {
                std::lock_guard lock(waiter->mutex);
            }
            waiter->ready.notify_all();
        });
        std::unique_lock lock(waiter->mutex);
        waiter->ready.wait_until(lock, deadline, [&] {
            return waiter->done || token.stop_requested() ||
                demand_state->cancelled.load();
        });
        if (!waiter->done || token.stop_requested() ||
            demand_state->cancelled.load() || !waiter->error.empty()) {
            waiter->cancelled = true;
            return std::nullopt;
        }
        return waiter->data;
    }

    bool enqueue(HttpCommand command) {
        if (shutting_down.load()) {
            return false;
        }
        std::lock_guard lock(commands_mutex);
        if (commands.size() >= maximum_http_commands) {
            return false;
        }
        commands.push_back(std::move(command));
        return true;
    }

    void enqueue_end(const std::uint64_t demand_id) {
        std::lock_guard lock(commands_mutex);
        commands.push_back({CommandType::end_demand, demand_id, {}, {}, 0, {}, {}});
    }

    void poll() {
        std::deque<HttpCommand> available;
        {
            std::lock_guard lock(commands_mutex);
            available.swap(commands);
        }
        std::unordered_set<std::string> changed_torrents;
        for (auto& command : available) {
            switch (command.type) {
            case CommandType::begin_demand:
                if (!revoked_torrents.contains(command.route.torrent_id) &&
                    route_is_active(command.route)) {
                    const auto id = command.route.torrent_id;
                    active_demands.insert_or_assign(command.demand_id, ActiveDemand{
                        command.demand_id,
                        std::move(command.route),
                        command.range,
                        std::move(command.demand_state),
                    });
                    changed_torrents.insert(id);
                } else if (command.demand_state) {
                    command.demand_state->cancelled.store(true);
                }
                break;
            case CommandType::end_demand: {
                const auto found = active_demands.find(command.demand_id);
                if (found != active_demands.end()) {
                    changed_torrents.insert(found->second.route.torrent_id);
                    active_demands.erase(found);
                }
                break;
            }
            case CommandType::read_piece:
                if (const auto demand = active_demands.find(command.demand_id);
                    demand != active_demands.end() && demand->second.state &&
                    !demand->second.state->cancelled.load()) {
                    demand->second.range = command.range;
                    changed_torrents.insert(demand->second.route.torrent_id);
                    process_piece_request(command);
                } else if (command.waiter) {
                    complete_waiter(command.waiter, {}, "stream demand is no longer active");
                }
                break;
            }
        }
        prune_cancelled_waiters();
        for (const auto& id : changed_torrents) {
            recompute_schedule(id);
        }
        expire_inactive_streams();
    }

    void expire_inactive_streams() {
        if (inactivity_timeout.count() == 0) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now < next_inactivity_check) {
            return;
        }
        const auto check_interval = std::clamp(
            inactivity_timeout / 2,
            std::chrono::milliseconds(25),
            std::chrono::milliseconds(1000)
        );
        next_inactivity_check = now + check_interval;
        std::vector<StoppedStream> expired;
        {
            std::lock_guard lock(routes_mutex);
            for (auto route_entry = routes.begin(); route_entry != routes.end();) {
                const auto& route = route_entry->second;
                const auto has_active_demand = std::ranges::any_of(
                    active_demands,
                    [&](const auto& demand) {
                        return demand.second.route.stream_id == route.stream_id;
                    }
                );
                if (has_active_demand ||
                    !route.activity->is_inactive(now, inactivity_timeout)) {
                    ++route_entry;
                    continue;
                }
                expired.push_back({route.torrent_id, route.stream_id});
                route_entry = routes.erase(route_entry);
                next_progress_sample = {};
            }
        }
        for (auto& stream : expired) {
            recompute_schedule(stream.torrent_id);
            expired_streams.push_back(std::move(stream));
        }
    }

    std::vector<StoppedStream> pop_expired_streams() {
        std::vector<StoppedStream> result;
        result.swap(expired_streams);
        return result;
    }

    bool route_is_active(const StreamRoute& route) {
        std::lock_guard lock(routes_mutex);
        const auto active = routes.find(route.target);
        return active != routes.end() &&
            active->second.stream_id == route.stream_id &&
            active->second.torrent_id == route.torrent_id;
    }

    void cancel_demand_waiters(
        const std::uint64_t demand_id,
        const std::string& error
    ) {
        for (auto entry = piece_waiters.begin(); entry != piece_waiters.end();) {
            auto& waiters = entry->second;
            for (const auto& waiter : waiters) {
                if (waiter->demand_id == demand_id) {
                    complete_waiter(waiter, {}, error);
                }
            }
            std::erase_if(waiters, [&](const std::shared_ptr<PieceWaiter>& waiter) {
                return waiter->demand_id == demand_id;
            });
            if (waiters.empty()) {
                entry = piece_waiters.erase(entry);
            } else {
                ++entry;
            }
        }
    }

    void process_piece_request(const HttpCommand& command) {
        if (!command.waiter) {
            return;
        }
        if (revoked_torrents.contains(command.route.torrent_id) ||
            !command.route.handle.is_valid() || !command.route.handle.in_session()) {
            complete_waiter(command.waiter, {}, "torrent is unavailable");
            return;
        }
        const auto metadata = command.route.handle.torrent_file();
        if (!metadata || command.piece >= static_cast<std::uint32_t>(metadata->num_pieces())) {
            complete_waiter(command.waiter, {}, "torrent piece is out of range");
            return;
        }
        const PieceKey key{command.route.torrent_id, command.piece};
        if (auto cached = piece_cache.get(key)) {
            last_ready_pieces.insert_or_assign(command.route.torrent_id, command.piece);
            complete_waiter(command.waiter, std::move(cached), {});
            return;
        }
        piece_waiters[key].push_back(command.waiter);
    }

    void recompute_schedule(const std::string& id) {
        std::map<std::uint32_t, scheduler::PriorityClass> combined;
        std::optional<lt::torrent_handle> handle;
        std::vector<scheduler::StreamDemand> demands;
        for (const auto& [demand_id, demand] : active_demands) {
            if (demand.route.torrent_id != id) {
                continue;
            }
            handle = demand.route.handle;
            demands.push_back({
                demand_id,
                demand.route.file_offset,
                demand.route.file_size,
                demand.route.piece_size,
                demand.range,
            });
        }
        scheduler::StreamDemandPlan plan;
        try {
            plan = scheduler::build_stream_demand_plan(
                std::move(demands),
                rolling_selection_bytes,
                critical_front_limit
            );
            for (const auto& priority : plan.pieces) {
                combined.insert_or_assign(priority.piece, priority.priority);
            }
        } catch (...) {
            return;
        }
        const auto previous = current_schedules.find(id);
        if (!handle.has_value() && previous != current_schedules.end()) {
            handle = previous->second.handle;
        }
        if (!handle.has_value() || !handle->is_valid()) {
            current_schedules.erase(id);
            return;
        }
        if (combined.empty() && previous != current_schedules.end() &&
            has_stream_for_torrent(id)) {
            combined = previous->second.priorities;
            for (auto& [piece, priority] : combined) {
                static_cast<void>(piece);
                if (priority == scheduler::PriorityClass::blocking) {
                    priority = scheduler::PriorityClass::playback;
                }
            }
        }

        if (previous != current_schedules.end()) {
            for (const auto& [piece, priority] : previous->second.priorities) {
                const auto replacement = combined.find(piece);
                if (priority == scheduler::PriorityClass::blocking &&
                    (replacement == combined.end() ||
                     replacement->second != scheduler::PriorityClass::blocking)) {
                    try {
                        handle->reset_piece_deadline(
                            lt::piece_index_t(static_cast<int>(piece))
                        );
                    } catch (...) {
                    }
                }
            }
        }

        try {
            if (previous == current_schedules.end()) {
                const auto metadata = handle->torrent_file();
                if (metadata) {
                    std::vector<lt::download_priority_t> priorities(
                        static_cast<std::size_t>(metadata->num_pieces()),
                        lt::dont_download
                    );
                    for (const auto& [piece, priority] : combined) {
                        if (piece < priorities.size()) {
                            priorities[piece] = download_priority_for(priority);
                        }
                    }
                    handle->prioritize_pieces(priorities);
                }
            } else {
                std::vector<std::pair<lt::piece_index_t, lt::download_priority_t>> updates;
                updates.reserve(previous->second.priorities.size() + combined.size());
                for (const auto& [piece, priority] : previous->second.priorities) {
                    static_cast<void>(priority);
                    if (!combined.contains(piece)) {
                        updates.emplace_back(
                            lt::piece_index_t(static_cast<int>(piece)),
                            lt::dont_download
                        );
                    }
                }
                for (const auto& [piece, priority] : combined) {
                    const auto old = previous->second.priorities.find(piece);
                    if (old == previous->second.priorities.end() ||
                        old->second != priority) {
                        updates.emplace_back(
                            lt::piece_index_t(static_cast<int>(piece)),
                            download_priority_for(priority)
                        );
                    }
                }
                if (!updates.empty()) {
                    handle->prioritize_pieces(updates);
                }
            }
        } catch (...) {
        }
        for (std::size_t ordinal = 0;
             ordinal < plan.blocking_deadline_order.size();
             ++ordinal) {
            const auto piece = plan.blocking_deadline_order[ordinal];
            const auto priority = combined.find(piece);
            if (priority == combined.end() ||
                priority->second != scheduler::PriorityClass::blocking) {
                continue;
            }
            try {
                const PieceKey key{id, piece};
                const auto flags = piece_waiters.contains(key)
                    ? lt::torrent_handle::alert_when_available
                    : lt::deadline_flags_t{};
                handle->set_piece_deadline(
                    lt::piece_index_t(static_cast<int>(piece)),
                    blocking_deadline_for(ordinal),
                    flags
                );
            } catch (const std::exception& error) {
                fail_piece(PieceKey{id, piece}, error.what());
            } catch (...) {
                fail_piece(PieceKey{id, piece}, "failed to request blocking piece");
            }
        }
        if (combined.empty()) {
            current_schedules.erase(id);
        } else {
            const auto revision = previous == current_schedules.end()
                ? std::uint64_t{1}
                : previous->second.revision +
                    static_cast<std::uint64_t>(
                        previous->second.priorities != combined ||
                        previous->second.blocking_deadline_order !=
                            plan.blocking_deadline_order ||
                        previous->second.focused_demand_id != plan.focused_demand_id
                    );
            current_schedules.insert_or_assign(
                id,
                CurrentSchedule{
                    *handle,
                    std::move(combined),
                    std::move(plan.blocking_deadline_order),
                    plan.focused_demand_id,
                    revision,
                }
            );
        }
    }

    void handle_read_piece(const lt::read_piece_alert& alert) {
        const PieceKey key{
            torrent_id(alert.handle),
            static_cast<std::uint32_t>(static_cast<int>(alert.piece)),
        };
        if (alert.error || !alert.buffer || alert.size <= 0) {
            fail_piece(key, alert.error ? alert.error.message() : "verified piece is empty");
            return;
        }
        auto data = std::make_shared<std::vector<char>>(
            alert.buffer.get(),
            alert.buffer.get() + alert.size
        );
        last_ready_pieces.insert_or_assign(key.torrent_id, key.piece);
        piece_cache.put(key, data);
        const auto found = piece_waiters.find(key);
        if (found == piece_waiters.end()) {
            return;
        }
        for (const auto& waiter : found->second) {
            complete_waiter(waiter, data, {});
        }
        piece_waiters.erase(found);
    }

    static void complete_waiter(
        const std::shared_ptr<PieceWaiter>& waiter,
        std::shared_ptr<std::vector<char>> data,
        std::string error
    ) {
        {
            std::lock_guard lock(waiter->mutex);
            if (waiter->cancelled || waiter->done) {
                return;
            }
            waiter->data = std::move(data);
            waiter->error = std::move(error);
            waiter->done = true;
        }
        waiter->ready.notify_all();
    }

    void fail_piece(const PieceKey& key, const std::string& error) {
        const auto found = piece_waiters.find(key);
        if (found == piece_waiters.end()) {
            return;
        }
        for (const auto& waiter : found->second) {
            complete_waiter(waiter, {}, error);
        }
        piece_waiters.erase(found);
    }

    void prune_cancelled_waiters() {
        for (auto entry = piece_waiters.begin(); entry != piece_waiters.end();) {
            auto& waiters = entry->second;
            std::erase_if(waiters, [](const std::shared_ptr<PieceWaiter>& waiter) {
                std::lock_guard lock(waiter->mutex);
                return waiter->cancelled;
            });
            if (waiters.empty()) {
                entry = piece_waiters.erase(entry);
            } else {
                ++entry;
            }
        }
    }

    void remove_torrent(const std::string& id) {
        revoked_torrents.insert(id);
        piece_cache.erase_torrent(id);
        last_ready_pieces.erase(id);
        {
            std::lock_guard lock(routes_mutex);
            std::erase_if(routes, [&](const auto& route) {
                return route.second.torrent_id == id;
            });
            next_progress_sample = {};
        }
        for (auto demand = active_demands.begin(); demand != active_demands.end();) {
            if (demand->second.route.torrent_id == id) {
                if (demand->second.state) {
                    demand->second.state->cancelled.store(true);
                }
                demand = active_demands.erase(demand);
            } else {
                ++demand;
            }
        }
        recompute_schedule(id);
        for (auto entry = piece_waiters.begin(); entry != piece_waiters.end();) {
            if (entry->first.torrent_id == id) {
                for (const auto& waiter : entry->second) {
                    complete_waiter(waiter, {}, "torrent was removed");
                }
                entry = piece_waiters.erase(entry);
            } else {
                ++entry;
            }
        }
    }

    bool has_stream_for_torrent(const std::string& id) {
        std::lock_guard lock(routes_mutex);
        return std::ranges::any_of(routes, [&](const auto& route) {
            return route.second.torrent_id == id;
        });
    }

    std::vector<std::uint32_t> blocking_pieces(const std::string& id) const {
        const auto schedule = current_schedules.find(id);
        if (schedule == current_schedules.end()) {
            return {};
        }
        return schedule->second.blocking_deadline_order;
    }

    std::string stop_stream(const std::string& stream_id) {
        std::string id;
        const auto target = "/stream/" + stream_id;
        {
            std::lock_guard lock(routes_mutex);
            const auto route = routes.find(target);
            if (route != routes.end()) {
                id = route->second.torrent_id;
                routes.erase(route);
                next_progress_sample = {};
            }
        }
        for (auto demand = active_demands.begin(); demand != active_demands.end();) {
            if (demand->second.route.stream_id != stream_id) {
                ++demand;
                continue;
            }
            if (id.empty()) {
                id = demand->second.route.torrent_id;
            }
            if (demand->second.state) {
                demand->second.state->cancelled.store(true);
            }
            cancel_demand_waiters(demand->first, "stream was stopped");
            demand = active_demands.erase(demand);
        }
        if (!id.empty()) {
            recompute_schedule(id);
        }
        return id;
    }

    BackendStats::Stream progress_for(const StreamRoute& route) const {
        BackendStats::Stream result{};
        result.stream_id = route.stream_id;
        result.file_index = route.file_index;
        result.file_size = route.file_size;
        result.delivered_bytes = route.counters->delivered_bytes.load(
            std::memory_order_relaxed
        );
        std::vector<const ActiveDemand*> demands;
        for (const auto& [demand_id, demand] : active_demands) {
            static_cast<void>(demand_id);
            if (demand.route.stream_id == route.stream_id) {
                demands.push_back(&demand);
            }
        }
        std::ranges::sort(demands, [](const ActiveDemand* left, const ActiveDemand* right) {
            return left->id > right->id;
        });
        result.active_demands = saturating_count(demands.size());
        if (!demands.empty()) {
            result.primary_demand_start = demands[0]->range.start;
            result.primary_demand_end = demands[0]->range.end;
        }
        if (demands.size() > 1) {
            result.secondary_demand_start = demands[1]->range.start;
            result.secondary_demand_end = demands[1]->range.end;
        }
        if (const auto schedule = current_schedules.find(route.torrent_id);
            schedule != current_schedules.end()) {
            result.scheduled_pieces = saturating_count(schedule->second.priorities.size());
            result.schedule_revision = schedule->second.revision;
            result.blocking_pieces = saturating_count(
                schedule->second.blocking_deadline_order.size()
            );
            if (!schedule->second.blocking_deadline_order.empty()) {
                result.primary_blocking_piece =
                    schedule->second.blocking_deadline_order[0];
            }
            if (schedule->second.blocking_deadline_order.size() > 1) {
                result.secondary_blocking_piece =
                    schedule->second.blocking_deadline_order[1];
            }
        }
        if (const auto ready = last_ready_pieces.find(route.torrent_id);
            ready != last_ready_pieces.end()) {
            result.last_ready_piece = ready->second;
        }
        if (route.file_size == 0 || !route.handle.is_valid() ||
            !route.handle.in_session()) {
            return result;
        }
        try {
            const auto metadata = route.handle.torrent_file();
            if (!metadata || metadata->piece_length() <= 0 ||
                route.file_size > std::numeric_limits<std::uint64_t>::max() -
                    route.file_offset) {
                return result;
            }
            const auto file_end = route.file_offset + route.file_size;
            const auto first_piece = route.file_offset / route.piece_size;
            const auto last_piece = (file_end - 1) / route.piece_size;
            if (last_piece >= static_cast<std::uint64_t>(metadata->num_pieces())) {
                return result;
            }
            const auto status = route.handle.status(lt::torrent_handle::query_pieces);
            bool contiguous = true;
            for (auto piece = first_piece; piece <= last_piece; ++piece) {
                const auto piece_number = static_cast<int>(piece);
                const auto index = lt::piece_index_t(piece_number);
                const auto ready = status.is_seeding ||
                    (piece_number < status.pieces.size() && status.pieces[index]);
                if (!ready) {
                    contiguous = false;
                    continue;
                }
                const auto piece_start = piece * route.piece_size;
                const auto raw_piece_bytes = metadata->piece_size(index);
                if (raw_piece_bytes <= 0) {
                    result.contiguous_ready_bytes = 0;
                    result.verified_file_bytes = 0;
                    return result;
                }
                const auto piece_bytes = static_cast<std::uint64_t>(raw_piece_bytes);
                const auto piece_end = piece_start + piece_bytes;
                const auto overlap_start = std::max(route.file_offset, piece_start);
                const auto overlap_end = std::min(file_end, piece_end);
                if (overlap_end <= overlap_start) {
                    continue;
                }
                const auto overlap = overlap_end - overlap_start;
                result.verified_file_bytes += overlap;
                if (contiguous) {
                    result.contiguous_ready_bytes += overlap;
                }
            }
        } catch (...) {
        }
        return result;
    }

    StreamBridgeStats statistics() {
        StreamBridgeStats result{};
        {
            std::lock_guard lock(routes_mutex);
            result.active_streams = saturating_count(routes.size());
            const auto now = std::chrono::steady_clock::now();
            if (now >= next_progress_sample) {
                stream_progress_cache.clear();
                stream_progress_cache.reserve(routes.size());
                for (const auto& [target, route] : routes) {
                    static_cast<void>(target);
                    stream_progress_cache.push_back(progress_for(route));
                }
                next_progress_sample = now + std::chrono::milliseconds(250);
            }
            result.streams = stream_progress_cache;
        }
        result.active_http_requests = server->active_request_count();
        result.active_demands = saturating_count(active_demands.size());
        std::uint64_t pending_reads = 0;
        for (const auto& [key, waiters] : piece_waiters) {
            static_cast<void>(key);
            pending_reads = std::min(
                pending_reads + static_cast<std::uint64_t>(waiters.size()),
                static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())
            );
        }
        result.pending_piece_reads = static_cast<std::uint32_t>(pending_reads);
        result.cache = piece_cache.stats();
        return result;
    }

    void shutdown() {
        if (shutting_down.exchange(true)) {
            return;
        }
        for (auto& [key, waiters] : piece_waiters) {
            static_cast<void>(key);
            for (const auto& waiter : waiters) {
                complete_waiter(waiter, {}, "engine is shutting down");
            }
        }
        piece_waiters.clear();
        server->stop();
        {
            std::lock_guard lock(commands_mutex);
            commands.clear();
        }
        for (const auto& [id, schedule] : current_schedules) {
            static_cast<void>(id);
            try {
                schedule.handle.clear_piece_deadlines();
            } catch (...) {
            }
        }
        current_schedules.clear();
        last_ready_pieces.clear();
        piece_cache.clear();
        for (auto& [id, demand] : active_demands) {
            static_cast<void>(id);
            if (demand.state) {
                demand.state->cancelled.store(true);
            }
        }
        active_demands.clear();
        expired_streams.clear();
        {
            std::lock_guard lock(routes_mutex);
            routes.clear();
        }
        stream_progress_cache.clear();
    }

    const std::uint64_t rolling_selection_bytes;
    const std::chrono::milliseconds inactivity_timeout;
    std::mutex routes_mutex;
    std::unordered_map<std::string, StreamRoute> routes;
    std::vector<BackendStats::Stream> stream_progress_cache;
    std::chrono::steady_clock::time_point next_progress_sample{};
    std::mutex commands_mutex;
    std::deque<HttpCommand> commands;
    std::atomic_uint64_t next_demand_id = 1;
    std::unordered_map<std::uint64_t, ActiveDemand> active_demands;
    std::unordered_map<std::string, CurrentSchedule> current_schedules;
    std::unordered_map<std::string, std::uint32_t> last_ready_pieces;
    std::unordered_map<PieceKey, std::vector<std::shared_ptr<PieceWaiter>>, PieceKeyHash>
        piece_waiters;
    std::unordered_set<std::string> revoked_torrents;
    std::vector<StoppedStream> expired_streams;
    std::chrono::steady_clock::time_point next_inactivity_check{};
    cache::VerifiedPieceCache piece_cache;
    std::atomic_bool shutting_down = false;
    std::unique_ptr<http::LoopbackServer> server;

    static std::uint32_t saturating_count(const std::size_t count) {
        return static_cast<std::uint32_t>(std::min(
            count,
            static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())
        ));
    }
};

LibtorrentStreamBridge::LibtorrentStreamBridge(
    const std::uint16_t requested_port,
    const std::uint64_t memory_capacity_bytes,
    const std::chrono::milliseconds inactivity_timeout
)
    : impl_(std::make_unique<Impl>(
          requested_port,
          memory_capacity_bytes,
          inactivity_timeout
      )) {
}

LibtorrentStreamBridge::~LibtorrentStreamBridge() = default;

PreparedStream LibtorrentStreamBridge::register_stream(
    const lt::torrent_handle& handle,
    const std::string& torrent_id,
    const std::uint32_t file_index,
    const TorrentFileInfo& file
) {
    return impl_->register_stream(handle, torrent_id, file_index, file);
}

void LibtorrentStreamBridge::poll() {
    impl_->poll();
}

std::vector<StoppedStream> LibtorrentStreamBridge::pop_expired_streams() {
    return impl_->pop_expired_streams();
}

void LibtorrentStreamBridge::handle_read_piece(const lt::read_piece_alert& alert) {
    impl_->handle_read_piece(alert);
}

std::string LibtorrentStreamBridge::stop_stream(const std::string& stream_id) {
    return impl_->stop_stream(stream_id);
}

bool LibtorrentStreamBridge::has_stream_for_torrent(const std::string& torrent_id) {
    return impl_->has_stream_for_torrent(torrent_id);
}

std::vector<std::uint32_t> LibtorrentStreamBridge::blocking_pieces(
    const std::string& torrent_id
) const {
    return impl_->blocking_pieces(torrent_id);
}

void LibtorrentStreamBridge::remove_torrent(const std::string& torrent_id) {
    impl_->remove_torrent(torrent_id);
}

StreamBridgeStats LibtorrentStreamBridge::statistics() {
    return impl_->statistics();
}

void LibtorrentStreamBridge::shutdown() {
    impl_->shutdown();
}

}
