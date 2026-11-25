#include "test_support.hpp"

#include "nuvio_engine/nuvio_engine.h"

#if defined(NUVIO_ENGINE_EXPECT_LIBTORRENT) && !defined(_WIN32)
#include "storage/range_release.hpp"
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/hasher.hpp>
#include <libtorrent/hex.hpp>
#include <libtorrent/posix_disk_io.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/write_resume_data.hpp>
#endif

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {

class CachePolicyDirectories {
public:
    explicit CachePolicyDirectories(
        const std::filesystem::path& parent = std::filesystem::temp_directory_path()
    ) {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = parent / ("nuvio-cache-policy-" + std::to_string(nonce));
        data_directory_ = (root_ / "data").string();
        cache_directory_ = (root_ / "cache").string();
        std::filesystem::create_directories(data_directory_);
        std::filesystem::create_directories(cache_directory_);
    }

    ~CachePolicyDirectories() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    void apply(nuvio_engine_config& config) const {
        config.data_directory = data_directory_.c_str();
        config.cache_directory = cache_directory_.c_str();
    }

    [[nodiscard]] std::filesystem::path payload(const std::string& torrent_id) const {
        return root_ / "cache" / "payload" / torrent_id;
    }

    [[nodiscard]] std::filesystem::path resume(
        const std::string& torrent_id,
        const bool piece_files = false
    ) const {
        return root_ / "data" / "nuvio-engine-state" / "resume" /
            (torrent_id + (piece_files ? ".pieces.resume" : ".resume"));
    }

private:
    std::filesystem::path root_;
    std::string data_directory_;
    std::string cache_directory_;
};

std::string torrent_data(const std::string& filename, const std::uint64_t length) {
    std::string result = "d4:infod6:lengthi" + std::to_string(length) + "e4:name" +
        std::to_string(filename.size()) + ":" + filename +
        "12:piece lengthi16384e6:pieces20:";
    result.append(20, '\0');
    result += "ee";
    return result;
}

nuvio_engine_event wait_for_event(
    nuvio_engine* const engine,
    const std::function<bool(const nuvio_engine_event&)>& predicate,
    bool* const received_cache_budget_error = nullptr
) {
    nuvio_engine_event event;
    nuvio_engine_event_init(&event);
    for (int attempt = 0; attempt < 500; ++attempt) {
        const auto status = nuvio_engine_poll_event(engine, &event);
        if (status == NUVIO_ENGINE_STATUS_NO_EVENT) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (status != NUVIO_ENGINE_STATUS_OK) {
            throw std::runtime_error("failed to poll engine event");
        }
        if (received_cache_budget_error != nullptr &&
            event.type == NUVIO_ENGINE_EVENT_TORRENT_ERROR &&
            std::string(event.message) ==
                "disk cache budget is exceeded by protected torrent data") {
            *received_cache_budget_error = true;
        }
        if (predicate(event)) {
            return event;
        }
    }
    throw std::runtime_error("timed out waiting for engine event");
}

std::string add_torrent(nuvio_engine* const engine, const std::string& data) {
    nuvio_engine_torrent_request request;
    nuvio_engine_torrent_request_init(&request);
    request.source_type = NUVIO_ENGINE_TORRENT_SOURCE_DATA;
    request.torrent_data = reinterpret_cast<const std::uint8_t*>(data.data());
    request.torrent_data_size = data.size();
    std::uint64_t request_id = 0;
    NUVIO_EXPECT_EQ(
        nuvio_engine_add_torrent(engine, &request, &request_id),
        NUVIO_ENGINE_STATUS_OK
    );
    const auto event = wait_for_event(engine, [request_id](const nuvio_engine_event& candidate) {
        return candidate.request_id == request_id &&
            candidate.type == NUVIO_ENGINE_EVENT_TORRENT_METADATA_READY;
    });
    return event.torrent_id;
}

nuvio_engine_event prepare_stream_event(nuvio_engine* const engine, const std::string& torrent_id) {
    nuvio_engine_stream_request request;
    nuvio_engine_stream_request_init(&request);
    request.torrent_id = torrent_id.c_str();
    request.file_index = 0;
    std::uint64_t request_id = 0;
    NUVIO_EXPECT_EQ(
        nuvio_engine_prepare_stream(engine, &request, &request_id),
        NUVIO_ENGINE_STATUS_OK
    );
    return wait_for_event(engine, [request_id](const nuvio_engine_event& candidate) {
        return candidate.request_id == request_id &&
            candidate.type == NUVIO_ENGINE_EVENT_STREAM_PREPARED;
    });
}

std::string prepare_stream(nuvio_engine* const engine, const std::string& torrent_id) {
    return prepare_stream_event(engine, torrent_id).stream_id;
}

#if defined(NUVIO_ENGINE_EXPECT_LIBTORRENT) && !defined(_WIN32)
NUVIO_TEST("active cache releases piece files on a volume without hole punching") {
    const auto* const volume = std::getenv("NUVIO_TEST_NO_HOLE_PUNCH_DIRECTORY");
    if (volume == nullptr) {
        return;
    }
    namespace lt = libtorrent;
    constexpr int piece_size = 1024 * 1024;
    constexpr int piece_count = 192;
    const CachePolicyDirectories directories(volume);
    nuvio_engine_config config;
    nuvio_engine_config_init(&config);
    directories.apply(config);
    config.disk_cache_capacity_bytes = 0;
    NUVIO_EXPECT_TRUE(!nuvio::storage::supports_range_release(config.cache_directory));

    lt::file_storage files;
    files.add_file("video.bin", std::int64_t{piece_size} * piece_count);
    lt::create_torrent creator(files, piece_size, lt::create_torrent::v1_only);
    const std::string contents(piece_size, 'v');
    const auto hash = lt::hasher(contents).final();
    for (int piece = 0; piece < piece_count; ++piece) {
        creator.set_hash(lt::piece_index_t(piece), hash);
    }
    const auto source = creator.generate_buf();
    lt::add_torrent_params resume;
    resume.ti = std::make_shared<lt::torrent_info>(source, lt::from_span);
    resume.info_hashes = resume.ti->info_hashes();
    resume.have_pieces.resize(piece_count, true);
    const auto id = lt::aux::to_hex(resume.info_hashes.v1);
    const auto payload = directories.payload(id).string();
    const auto piece_directory = lt::nuvio_piece_directory(payload);
    std::filesystem::create_directories(piece_directory);
    const auto orphan = lt::nuvio_piece_path(payload, lt::piece_index_t(piece_count));
    {
        std::ofstream output(orphan, std::ios::binary);
        output << "unverified";
        NUVIO_EXPECT_TRUE(bool(output));
    }
    for (int piece = 0; piece < piece_count; ++piece) {
        std::ofstream output(lt::nuvio_piece_path(payload, lt::piece_index_t(piece)), std::ios::binary);
        output.write(contents.data(), std::streamsize(contents.size()));
        NUVIO_EXPECT_TRUE(bool(output));
    }
    const auto resume_path = directories.resume(id, true);
    std::filesystem::create_directories(resume_path.parent_path());
    const auto resume_data = lt::write_resume_data_buf(resume);
    {
        std::ofstream output(resume_path, std::ios::binary);
        output.write(resume_data.data(), std::streamsize(resume_data.size()));
        NUVIO_EXPECT_TRUE(bool(output));
    }

    nuvio_engine* raw_engine = nullptr;
    NUVIO_EXPECT_EQ(nuvio_engine_create(&config, &raw_engine), NUVIO_ENGINE_STATUS_OK);
    const std::unique_ptr<nuvio_engine, decltype(&nuvio_engine_destroy)> engine(
        raw_engine, nuvio_engine_destroy
    );
    NUVIO_EXPECT_EQ(add_torrent(engine.get(), {source.begin(), source.end()}), id);
    NUVIO_EXPECT_TRUE(!std::filesystem::exists(orphan));
    const std::string url = prepare_stream_event(engine.get(), id).stream_url;
    const auto port = url.find(':', std::string_view("http://").size());
    const auto path = url.find('/', port);
    NUVIO_EXPECT_TRUE(port != std::string::npos && path != std::string::npos);
    const auto read_range = [&](const std::string& range) {
        const auto response = nuvio::test::send_http_request(
            static_cast<std::uint16_t>(std::stoi(url.substr(port + 1, path - port - 1))),
            "GET " + url.substr(path) + " HTTP/1.1\r\nHost: localhost\r\nRange: bytes=" +
                range + "\r\nConnection: close\r\n\r\n"
        );
        NUVIO_EXPECT_TRUE(response.starts_with("HTTP/1.1 206"));
        NUVIO_EXPECT_TRUE(response.ends_with("\r\n\r\nvvvv"));
    };

    read_range("50331648-50331651");
    std::size_t retained = piece_count;
    for (int attempt = 0; attempt < 200 && retained > 144; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        retained = static_cast<std::size_t>(std::distance(
            std::filesystem::directory_iterator(piece_directory),
            std::filesystem::directory_iterator{}
        ));
    }
    NUVIO_EXPECT_TRUE(retained > 0 && retained <= 144);
    read_range("1048576-1048579");
}
#endif

void write_payload(const std::filesystem::path& directory, const std::string& filename) {
    std::filesystem::create_directories(directory);
    std::ofstream output(directory / filename, std::ios::binary);
    output << "test";
}

}

NUVIO_TEST("payload cached before startup survives until its torrent is added") {
    CachePolicyDirectories directories;
    const std::string torrent_id = "13ade5f13f4e7ce4021a3cc82f72e504b9ef35ac";
    write_payload(directories.payload(torrent_id), "test.bin");
    nuvio_engine_config config;
    nuvio_engine_config_init(&config);
    directories.apply(config);
    config.disk_cache_capacity_bytes = 3;
    nuvio_engine* engine = nullptr;
    NUVIO_EXPECT_EQ(nuvio_engine_create(&config, &engine), NUVIO_ENGINE_STATUS_OK);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    constexpr std::array<unsigned char, 20> piece_hash{
        0xa9, 0x4a, 0x8f, 0xe5, 0xcc, 0xb1, 0x9b, 0xa6, 0x1c, 0x4c,
        0x08, 0x73, 0xd3, 0x91, 0xe9, 0x87, 0x98, 0x2f, 0xbb, 0xd3,
    };
    std::string data =
        "d4:infod6:lengthi4e4:name8:test.bin12:piece lengthi16384e6:pieces20:";
    data.append(piece_hash.begin(), piece_hash.end());
    data += "ee";
    NUVIO_EXPECT_EQ(add_torrent(engine, data), torrent_id);
    const auto stream_id = prepare_stream(engine, torrent_id);

    nuvio_engine_stream_stats stream_stats;
    nuvio_engine_stream_stats_init(&stream_stats);
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (nuvio_engine_get_stream_stats(engine, stream_id.c_str(), &stream_stats) ==
                NUVIO_ENGINE_STATUS_OK &&
            stream_stats.verified_file_bytes == 4) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    NUVIO_EXPECT_TRUE(std::filesystem::exists(directories.payload(torrent_id) / "test.bin"));
    NUVIO_EXPECT_EQ(stream_stats.verified_file_bytes, std::uint64_t(4));
    nuvio_engine_destroy(engine);
}

NUVIO_TEST("protected cache pressure remains nonfatal during an active stream") {
    CachePolicyDirectories directories;
    nuvio_engine_config config;
    nuvio_engine_config_init(&config);
    directories.apply(config);
    config.disk_cache_capacity_bytes = 3;
    nuvio_engine* engine = nullptr;
    NUVIO_EXPECT_EQ(nuvio_engine_create(&config, &engine), NUVIO_ENGINE_STATUS_OK);

    const auto active_id = add_torrent(engine, torrent_data("test.bin", 4));
    write_payload(directories.payload(active_id), "test.bin");
    const auto stream_id = prepare_stream(engine, active_id);
    const auto disposable_id = add_torrent(engine, torrent_data("other.bin", 1));
    std::uint64_t remove_request_id = 0;
    NUVIO_EXPECT_EQ(
        nuvio_engine_remove_torrent(engine, disposable_id.c_str(), &remove_request_id),
        NUVIO_ENGINE_STATUS_OK
    );

    bool received_cache_budget_error = false;
    static_cast<void>(wait_for_event(
        engine,
        [remove_request_id](const nuvio_engine_event& event) {
            return event.request_id == remove_request_id &&
                event.type == NUVIO_ENGINE_EVENT_TORRENT_REMOVED;
        },
        &received_cache_budget_error
    ));
    nuvio_engine_stats stats;
    nuvio_engine_stats_init(&stats);
    for (int attempt = 0; attempt < 100; ++attempt) {
        NUVIO_EXPECT_EQ(nuvio_engine_get_stats(engine, &stats), NUVIO_ENGINE_STATUS_OK);
        if (stats.disk_cache_over_budget != 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    nuvio_engine_event event;
    nuvio_engine_event_init(&event);
    for (int attempt = 0; attempt < 50; ++attempt) {
        const auto status = nuvio_engine_poll_event(engine, &event);
        if (status == NUVIO_ENGINE_STATUS_NO_EVENT) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        NUVIO_EXPECT_EQ(status, NUVIO_ENGINE_STATUS_OK);
        if (event.type == NUVIO_ENGINE_EVENT_TORRENT_ERROR &&
            std::string(event.message) ==
                "disk cache budget is exceeded by protected torrent data") {
            received_cache_budget_error = true;
        }
    }

    nuvio_engine_stream_stats stream_stats;
    nuvio_engine_stream_stats_init(&stream_stats);
    NUVIO_EXPECT_EQ(
        nuvio_engine_get_stream_stats(engine, stream_id.c_str(), &stream_stats),
        NUVIO_ENGINE_STATUS_OK
    );
    NUVIO_EXPECT_EQ(stats.disk_cache_used_bytes, std::uint64_t(4));
    NUVIO_EXPECT_EQ(stats.disk_cache_protected_bytes, std::uint64_t(4));
    NUVIO_EXPECT_EQ(stats.disk_cache_over_budget, std::uint8_t(1));
    NUVIO_EXPECT_TRUE(!received_cache_budget_error);
    nuvio_engine_destroy(engine);
}

NUVIO_TEST("reclaim unloads a stopped torrent before evicting its payload") {
    CachePolicyDirectories directories;
    nuvio_engine_config config;
    nuvio_engine_config_init(&config);
    directories.apply(config);
    config.disk_cache_capacity_bytes = 3;
    nuvio_engine* engine = nullptr;
    NUVIO_EXPECT_EQ(nuvio_engine_create(&config, &engine), NUVIO_ENGINE_STATUS_OK);

    const auto torrent_id = add_torrent(engine, torrent_data("test.bin", 4));
    const auto payload = directories.payload(torrent_id);
    write_payload(payload, "test.bin");
    const auto stream_id = prepare_stream(engine, torrent_id);
    std::uint64_t stop_request_id = 0;
    NUVIO_EXPECT_EQ(
        nuvio_engine_stop_stream(engine, stream_id.c_str(), &stop_request_id),
        NUVIO_ENGINE_STATUS_OK
    );
    static_cast<void>(wait_for_event(engine, [stop_request_id](const nuvio_engine_event& event) {
        return event.request_id == stop_request_id &&
            event.type == NUVIO_ENGINE_EVENT_STREAM_STOPPED;
    }));

    std::uint64_t reclaim_request_id = 0;
    NUVIO_EXPECT_EQ(
        nuvio_engine_reclaim_disk_cache(engine, 0, &reclaim_request_id),
        NUVIO_ENGINE_STATUS_OK
    );
    const auto reclaimed = wait_for_event(
        engine,
        [reclaim_request_id](const nuvio_engine_event& event) {
            return event.request_id == reclaim_request_id &&
                event.type == NUVIO_ENGINE_EVENT_DISK_CACHE_RECLAIMED;
        }
    );

    nuvio_engine_stats stats;
    nuvio_engine_stats_init(&stats);
    NUVIO_EXPECT_EQ(nuvio_engine_get_stats(engine, &stats), NUVIO_ENGINE_STATUS_OK);
    NUVIO_EXPECT_EQ(std::string(reclaimed.message), std::string("disk cache reclaim target reached"));
    NUVIO_EXPECT_TRUE(!std::filesystem::exists(payload));
    NUVIO_EXPECT_TRUE(std::filesystem::is_regular_file(directories.resume(torrent_id)));
    NUVIO_EXPECT_EQ(stats.active_torrents, std::uint32_t(0));
    NUVIO_EXPECT_EQ(stats.disk_cache_used_bytes, std::uint64_t(0));
    NUVIO_EXPECT_EQ(stats.disk_cache_protected_bytes, std::uint64_t(0));
    nuvio_engine_destroy(engine);
}

NUVIO_TEST("shutdown trims payload of a streamed torrent to the disk cache budget") {
    CachePolicyDirectories directories;
    nuvio_engine_config config;
    nuvio_engine_config_init(&config);
    directories.apply(config);
    config.disk_cache_capacity_bytes = 3;
    nuvio_engine* engine = nullptr;
    NUVIO_EXPECT_EQ(nuvio_engine_create(&config, &engine), NUVIO_ENGINE_STATUS_OK);

    const auto torrent_id = add_torrent(engine, torrent_data("test.bin", 4));
    const auto payload = directories.payload(torrent_id);
    write_payload(payload, "test.bin");
    static_cast<void>(prepare_stream(engine, torrent_id));
    nuvio_engine_destroy(engine);

    NUVIO_EXPECT_TRUE(!std::filesystem::exists(payload));
    NUVIO_EXPECT_TRUE(std::filesystem::is_regular_file(directories.resume(torrent_id)));
}

NUVIO_TEST("shutdown keeps payload that fits the disk cache budget") {
    CachePolicyDirectories directories;
    nuvio_engine_config config;
    nuvio_engine_config_init(&config);
    directories.apply(config);
    config.disk_cache_capacity_bytes = 1024 * 1024;
    nuvio_engine* engine = nullptr;
    NUVIO_EXPECT_EQ(nuvio_engine_create(&config, &engine), NUVIO_ENGINE_STATUS_OK);

    const auto torrent_id = add_torrent(engine, torrent_data("test.bin", 4));
    const auto payload = directories.payload(torrent_id);
    write_payload(payload, "test.bin");
    static_cast<void>(prepare_stream(engine, torrent_id));
    nuvio_engine_destroy(engine);

    NUVIO_EXPECT_TRUE(std::filesystem::is_regular_file(payload / "test.bin"));
}

NUVIO_TEST("memory storage streams a torrent without creating payload files") {
    CachePolicyDirectories directories;
    nuvio_engine_config config;
    nuvio_engine_config_init(&config);
    directories.apply(config);
    config.storage_mode = NUVIO_ENGINE_STORAGE_MEMORY;
    nuvio_engine* engine = nullptr;
    NUVIO_EXPECT_EQ(nuvio_engine_create(&config, &engine), NUVIO_ENGINE_STATUS_OK);

    const auto torrent_id = add_torrent(engine, torrent_data("test.bin", 4));
    static_cast<void>(prepare_stream(engine, torrent_id));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    const auto payload = directories.payload(torrent_id);
    NUVIO_EXPECT_TRUE(!std::filesystem::exists(payload / "test.bin"));
    nuvio_engine_destroy(engine);
    NUVIO_EXPECT_TRUE(!std::filesystem::exists(payload));
}

NUVIO_TEST("memory storage purges payload left by disk storage") {
    CachePolicyDirectories directories;
    const std::string torrent_id = "13ade5f13f4e7ce4021a3cc82f72e504b9ef35ac";
    write_payload(directories.payload(torrent_id), "test.bin");
    nuvio_engine_config config;
    nuvio_engine_config_init(&config);
    directories.apply(config);
    config.storage_mode = NUVIO_ENGINE_STORAGE_MEMORY;
    nuvio_engine* engine = nullptr;
    NUVIO_EXPECT_EQ(nuvio_engine_create(&config, &engine), NUVIO_ENGINE_STATUS_OK);

    for (int attempt = 0; attempt < 300; ++attempt) {
        if (!std::filesystem::exists(directories.payload(torrent_id))) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    NUVIO_EXPECT_TRUE(!std::filesystem::exists(directories.payload(torrent_id)));
    nuvio_engine_destroy(engine);
}
