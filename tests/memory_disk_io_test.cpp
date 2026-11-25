#include "test_support.hpp"

#include "storage/memory_piece_store.hpp"
#include "torrent/memory_disk_io.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/address.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/bencode.hpp>
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/posix_disk_io.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_status.hpp>

namespace {

constexpr int piece_length = 16 * 1024;

class MemoryDiskIoDirectories {
public:
    MemoryDiskIoDirectories() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = std::filesystem::temp_directory_path() /
            ("nuvio-memory-disk-io-" + std::to_string(nonce));
        std::filesystem::create_directories(seeder());
        std::filesystem::create_directories(leecher());
    }

    ~MemoryDiskIoDirectories() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    [[nodiscard]] std::filesystem::path seeder() const {
        return root_ / "seeder";
    }

    [[nodiscard]] std::filesystem::path leecher() const {
        return root_ / "leecher";
    }

private:
    std::filesystem::path root_;
};

lt::settings_pack loopback_settings() {
    lt::settings_pack settings;
    settings.set_str(lt::settings_pack::listen_interfaces, "127.0.0.1:0");
    settings.set_bool(lt::settings_pack::enable_dht, false);
    settings.set_bool(lt::settings_pack::enable_lsd, false);
    settings.set_bool(lt::settings_pack::enable_upnp, false);
    settings.set_bool(lt::settings_pack::enable_natpmp, false);
    settings.set_bool(lt::settings_pack::enable_outgoing_utp, false);
    settings.set_bool(lt::settings_pack::enable_incoming_utp, false);
    settings.set_int(
        lt::settings_pack::alert_mask,
        lt::alert_category::status | lt::alert_category::storage
    );
    return settings;
}

template <typename Predicate>
void wait_until(const Predicate& predicate, const char* description) {
    for (int attempt = 0; attempt < 600; ++attempt) {
        if (predicate()) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    throw std::runtime_error(std::string("timed out waiting for ") + description);
}

std::size_t regular_files(const std::filesystem::path& root) {
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) {
            ++count;
        }
    }
    return count;
}

}

NUVIO_TEST("memory disk io downloads and verifies a torrent without writing payload files") {
    MemoryDiskIoDirectories directories;
    std::string content;
    for (int index = 0; index < 3 * piece_length + 123; ++index) {
        content.push_back(static_cast<char>((index * 31 + 7) % 251));
    }
    {
        std::ofstream output(directories.seeder() / "payload.bin", std::ios::binary);
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    lt::file_storage files;
    lt::add_files(files, (directories.seeder() / "payload.bin").string());
    lt::create_torrent creator(files, piece_length);
    lt::set_piece_hashes(creator, directories.seeder().string());
    std::vector<char> encoded;
    lt::bencode(std::back_inserter(encoded), creator.generate());
    const auto metadata = std::make_shared<lt::torrent_info>(encoded, lt::from_span);

    lt::session_params seeder_params(loopback_settings());
    seeder_params.disk_io_constructor = lt::posix_disk_io_constructor;
    lt::session seeder(std::move(seeder_params));
    lt::add_torrent_params seeded;
    seeded.ti = metadata;
    seeded.save_path = directories.seeder().string();
    const auto seeding = seeder.add_torrent(std::move(seeded));
    wait_until([&] { return seeding.status().is_seeding; }, "seeder to verify its payload");

    auto store = std::make_shared<nuvio::storage::MemoryPieceStore>();
    lt::session_params leecher_params(loopback_settings());
    leecher_params.disk_io_constructor = nuvio::torrent::memory_disk_io_constructor(store);
    lt::session leecher(std::move(leecher_params));
    lt::add_torrent_params wanted;
    wanted.ti = metadata;
    wanted.save_path = directories.leecher().string();
    const auto downloading = leecher.add_torrent(std::move(wanted));
    downloading.connect_peer(
        lt::tcp::endpoint(lt::make_address("127.0.0.1"), seeder.listen_port())
    );
    wait_until([&] { return downloading.status().is_seeding; }, "memory leecher to finish");

    NUVIO_EXPECT_EQ(store->used_bytes(), std::uint64_t(content.size()));
    NUVIO_EXPECT_EQ(store->stored_pieces(), std::uint64_t(4));
    NUVIO_EXPECT_EQ(regular_files(directories.leecher()), std::size_t(0));

    downloading.read_piece(lt::piece_index_t(3));
    std::string last_piece;
    wait_until([&] {
        std::vector<lt::alert*> alerts;
        leecher.pop_alerts(&alerts);
        for (auto* alert : alerts) {
            if (const auto* read = lt::alert_cast<lt::read_piece_alert>(alert)) {
                if (!read->error) {
                    last_piece.assign(read->buffer.get(), static_cast<std::size_t>(read->size));
                }
                return true;
            }
        }
        return false;
    }, "memory piece read");
    NUVIO_EXPECT_EQ(last_piece, content.substr(3 * piece_length));
}
