#ifndef NUVIO_ENGINE_MEMORY_PIECE_STORE_HPP
#define NUVIO_ENGINE_MEMORY_PIECE_STORE_HPP

#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nuvio::storage {

class MemoryPieceStore {
public:
    class Torrent;

    using Piece = std::shared_ptr<const std::vector<char>>;

    [[nodiscard]] std::shared_ptr<Torrent> attach(const std::string& key);
    void detach(const std::shared_ptr<Torrent>& torrent);

    [[nodiscard]] bool write(
        Torrent& torrent,
        std::uint32_t piece,
        std::uint32_t piece_size,
        std::uint32_t offset,
        std::span<const char> data
    );
    [[nodiscard]] bool read(
        const Torrent& torrent,
        std::uint32_t piece,
        std::uint32_t offset,
        std::span<char> destination
    ) const;
    [[nodiscard]] Piece piece(const Torrent& torrent, std::uint32_t piece) const;
    void erase(Torrent& torrent, std::uint32_t piece);
    void release(const std::string& key, std::uint32_t piece);

    [[nodiscard]] std::uint64_t used_bytes() const;
    [[nodiscard]] std::uint64_t stored_pieces() const;

private:
    void erase_locked(Torrent& torrent, std::uint32_t piece);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::weak_ptr<Torrent>> torrents_;
    std::uint64_t used_bytes_ = 0;
    std::uint64_t stored_pieces_ = 0;
};

class MemoryPieceStore::Torrent {
public:
    explicit Torrent(std::string key) : key_(std::move(key)) {}

    [[nodiscard]] const std::string& key() const {
        return key_;
    }

private:
    friend class MemoryPieceStore;

    std::string key_;
    std::unordered_map<std::uint32_t, std::shared_ptr<std::vector<char>>> pieces_;
};

}

#endif
