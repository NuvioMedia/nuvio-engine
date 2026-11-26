#include "torrent/memory_disk_io.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>
#include <libtorrent/aux_/vector.hpp>
#include <libtorrent/error_code.hpp>
#include <libtorrent/file_storage.hpp>
#include <libtorrent/hasher.hpp>
#include <libtorrent/hex.hpp>
#include <libtorrent/operations.hpp>
#include <libtorrent/peer_request.hpp>
#include <libtorrent/storage_defs.hpp>

namespace nuvio::torrent {
namespace {

lt::storage_error missing_piece_error() {
    lt::storage_error error;
    error.ec = boost::asio::error::eof;
    error.operation = lt::operation_t::file_read;
    return error;
}

lt::storage_error unsupported_error() {
    lt::storage_error error;
    error.ec = lt::error_code(
        boost::system::errc::operation_not_supported,
        lt::system_category()
    );
    error.operation = lt::operation_t::file;
    return error;
}

struct MemoryTorrent {
    std::shared_ptr<storage::MemoryPieceStore::Torrent> pieces;
    lt::file_storage const* files = nullptr;
};

class MemoryDiskIo final : public lt::disk_interface, public lt::buffer_allocator_interface {
public:
    MemoryDiskIo(lt::io_context& context, std::shared_ptr<storage::MemoryPieceStore> store)
        : context_(context),
          store_(std::move(store)) {
    }

    ~MemoryDiskIo() override {
        for (auto& torrent : torrents_) {
            if (torrent.pieces) {
                store_->detach(torrent.pieces);
            }
        }
    }

    lt::storage_holder new_torrent(
        lt::storage_params const& params,
        std::shared_ptr<void> const&
    ) override {
        const auto index = free_slots_.empty() ? torrents_.end_index() : pop_free_slot();
        MemoryTorrent torrent{
            store_->attach(lt::aux::to_hex(params.info_hash)),
            &params.files,
        };
        if (index == torrents_.end_index()) {
            torrents_.emplace_back(std::move(torrent));
        } else {
            torrents_[index] = std::move(torrent);
        }
        return lt::storage_holder(index, *this);
    }

    void remove_torrent(const lt::storage_index_t index) override {
        store_->detach(torrents_[index].pieces);
        torrents_[index] = MemoryTorrent{};
        free_slots_.push_back(index);
    }

    void abort(bool) override {}

    void async_read(
        const lt::storage_index_t index,
        lt::peer_request const& request,
        std::function<void(lt::disk_buffer_holder, lt::storage_error const&)> handler,
        lt::disk_job_flags_t
    ) override {
        auto& torrent = torrents_[index];
        char* buffer = request.length > 0 ? new char[static_cast<std::size_t>(request.length)]
                                          : nullptr;
        lt::disk_buffer_holder holder(*this, buffer, buffer == nullptr ? 0 : request.length);
        lt::storage_error error;
        if (request.start < 0 || request.length < 0 ||
            !store_->read(
                *torrent.pieces,
                piece_number(request.piece),
                static_cast<std::uint32_t>(request.start),
                {buffer, static_cast<std::size_t>(request.length)}
            )) {
            error = missing_piece_error();
            holder.reset();
        }
        boost::asio::post(
            context_,
            [h = std::move(handler), b = std::move(holder), error]() mutable {
                h(std::move(b), error);
            }
        );
    }

    bool async_write(
        const lt::storage_index_t index,
        lt::peer_request const& request,
        char const* buffer,
        std::shared_ptr<lt::disk_observer>,
        std::function<void(lt::storage_error const&)> handler,
        lt::disk_job_flags_t
    ) override {
        auto& torrent = torrents_[index];
        lt::storage_error error;
        const auto size = torrent.files->piece_size(request.piece);
        if (request.start < 0 || request.length < 0 || size <= 0 ||
            !store_->write(
                *torrent.pieces,
                piece_number(request.piece),
                static_cast<std::uint32_t>(size),
                static_cast<std::uint32_t>(request.start),
                {buffer, static_cast<std::size_t>(request.length)}
            )) {
            error.ec = lt::error_code(boost::system::errc::invalid_argument, lt::system_category());
            error.operation = lt::operation_t::file_write;
        }
        boost::asio::post(context_, [h = std::move(handler), error] { h(error); });
        return false;
    }

    void async_hash(
        const lt::storage_index_t index,
        const lt::piece_index_t piece,
        lt::span<lt::sha256_hash> block_hashes,
        const lt::disk_job_flags_t flags,
        std::function<void(lt::piece_index_t, lt::sha1_hash const&, lt::storage_error const&)>
            handler
    ) override {
        auto& torrent = torrents_[index];
        lt::storage_error error;
        lt::sha1_hash hash;
        const auto data = store_->piece(*torrent.pieces, piece_number(piece));
        if (!data) {
            error = missing_piece_error();
        } else {
            const std::span<const char> bytes(*data);
            if (flags & lt::disk_interface::v1_hash) {
                const auto size = std::min<std::size_t>(
                    bytes.size(),
                    static_cast<std::size_t>(torrent.files->piece_size(piece))
                );
                hash = lt::hasher(bytes.data(), static_cast<int>(size)).final();
            }
            if (!block_hashes.empty()) {
                const auto size = std::min<std::size_t>(
                    bytes.size(),
                    static_cast<std::size_t>(torrent.files->piece_size2(piece))
                );
                const auto blocks = torrent.files->blocks_in_piece2(piece);
                std::size_t offset = 0;
                for (int block = 0; block < blocks && offset < size; ++block) {
                    const auto length = std::min<std::size_t>(
                        static_cast<std::size_t>(lt::default_block_size),
                        size - offset
                    );
                    block_hashes[block] = lt::hasher256(
                        bytes.data() + offset,
                        static_cast<int>(length)
                    ).final();
                    offset += length;
                }
            }
        }
        boost::asio::post(context_, [h = std::move(handler), piece, hash, error] {
            h(piece, hash, error);
        });
    }

    void async_hash2(
        const lt::storage_index_t index,
        const lt::piece_index_t piece,
        const int offset,
        lt::disk_job_flags_t,
        std::function<void(lt::piece_index_t, lt::sha256_hash const&, lt::storage_error const&)>
            handler
    ) override {
        auto& torrent = torrents_[index];
        lt::storage_error error;
        lt::sha256_hash hash;
        const auto data = store_->piece(*torrent.pieces, piece_number(piece));
        const auto size = std::min<std::size_t>(
            data ? data->size() : 0,
            static_cast<std::size_t>(torrent.files->piece_size2(piece))
        );
        if (!data || offset < 0 || static_cast<std::size_t>(offset) >= size) {
            error = missing_piece_error();
        } else {
            const auto start = static_cast<std::size_t>(offset);
            const auto length = std::min<std::size_t>(
                static_cast<std::size_t>(lt::default_block_size),
                size - start
            );
            hash = lt::hasher256(data->data() + start, static_cast<int>(length)).final();
        }
        boost::asio::post(context_, [h = std::move(handler), piece, hash, error] {
            h(piece, hash, error);
        });
    }

    void async_move_storage(
        lt::storage_index_t,
        std::string path,
        lt::move_flags_t,
        std::function<void(lt::status_t, std::string const&, lt::storage_error const&)> handler
    ) override {
        boost::asio::post(context_, [h = std::move(handler), p = std::move(path)] {
            h(lt::status_t::fatal_disk_error, p, unsupported_error());
        });
    }

    void async_release_files(lt::storage_index_t, std::function<void()> handler) override {
        if (handler) {
            boost::asio::post(context_, std::move(handler));
        }
    }

    void async_check_files(
        lt::storage_index_t,
        lt::add_torrent_params const*,
        lt::aux::vector<std::string, lt::file_index_t>,
        std::function<void(lt::status_t, lt::storage_error const&)> handler
    ) override {
        boost::asio::post(context_, [h = std::move(handler)] {
            h(lt::status_t::no_error, lt::storage_error());
        });
    }

    void async_stop_torrent(lt::storage_index_t, std::function<void()> handler) override {
        if (handler) {
            boost::asio::post(context_, std::move(handler));
        }
    }

    void async_rename_file(
        lt::storage_index_t,
        const lt::file_index_t file,
        std::string name,
        std::function<void(std::string const&, lt::file_index_t, lt::storage_error const&)>
            handler
    ) override {
        boost::asio::post(context_, [h = std::move(handler), n = std::move(name), file] {
            h(n, file, lt::storage_error());
        });
    }

    void async_delete_files(
        const lt::storage_index_t index,
        lt::remove_flags_t,
        std::function<void(lt::storage_error const&)> handler
    ) override {
        auto& torrent = torrents_[index];
        if (torrent.pieces) {
            const auto key = torrent.pieces->key();
            store_->detach(torrent.pieces);
            torrent.pieces = store_->attach(key);
        }
        boost::asio::post(context_, [h = std::move(handler)] { h(lt::storage_error()); });
    }

    void async_set_file_priority(
        lt::storage_index_t,
        lt::aux::vector<lt::download_priority_t, lt::file_index_t> priorities,
        std::function<void(
            lt::storage_error const&,
            lt::aux::vector<lt::download_priority_t, lt::file_index_t>
        )> handler
    ) override {
        boost::asio::post(context_, [h = std::move(handler), p = std::move(priorities)]() mutable {
            h(lt::storage_error(), std::move(p));
        });
    }

    void async_clear_piece(
        const lt::storage_index_t index,
        const lt::piece_index_t piece,
        std::function<void(lt::piece_index_t)> handler
    ) override {
        store_->erase(*torrents_[index].pieces, piece_number(piece));
        boost::asio::post(context_, [h = std::move(handler), piece] { h(piece); });
    }

    void update_stats_counters(lt::counters&) const override {}

    std::vector<lt::open_file_state> get_status(lt::storage_index_t) const override {
        return {};
    }

    void submit_jobs() override {}

    void settings_updated() override {}

    void free_disk_buffer(char* buffer) override {
        delete[] buffer;
    }

private:
    static std::uint32_t piece_number(const lt::piece_index_t piece) {
        return static_cast<std::uint32_t>(static_cast<int>(piece));
    }

    lt::storage_index_t pop_free_slot() {
        const auto index = free_slots_.back();
        free_slots_.pop_back();
        return index;
    }

    lt::io_context& context_;
    std::shared_ptr<storage::MemoryPieceStore> store_;
    lt::aux::vector<MemoryTorrent, lt::storage_index_t> torrents_;
    std::vector<lt::storage_index_t> free_slots_;
};

}

DiskIoConstructor memory_disk_io_constructor(std::shared_ptr<storage::MemoryPieceStore> store) {
    return [store = std::move(store)](
               lt::io_context& context,
               const lt::settings_interface&,
               lt::counters&
           ) -> std::unique_ptr<lt::disk_interface> {
        return std::make_unique<MemoryDiskIo>(context, store);
    };
}

}
