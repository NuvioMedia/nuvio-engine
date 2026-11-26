#include "storage/memory_piece_store.hpp"

#include <cstring>

namespace nuvio::storage {

std::shared_ptr<MemoryPieceStore::Torrent> MemoryPieceStore::attach(const std::string& key) {
    auto torrent = std::make_shared<Torrent>(key);
    std::lock_guard lock(mutex_);
    torrents_[key] = torrent;
    return torrent;
}

void MemoryPieceStore::detach(const std::shared_ptr<Torrent>& torrent) {
    if (!torrent) {
        return;
    }
    std::lock_guard lock(mutex_);
    while (!torrent->pieces_.empty()) {
        erase_locked(*torrent, torrent->pieces_.begin()->first);
    }
    const auto registered = torrents_.find(torrent->key_);
    if (registered != torrents_.end()) {
        const auto current = registered->second.lock();
        if (!current || current == torrent) {
            torrents_.erase(registered);
        }
    }
}

bool MemoryPieceStore::write(
    Torrent& torrent,
    const std::uint32_t piece,
    const std::uint32_t piece_size,
    const std::uint32_t offset,
    const std::span<const char> data
) {
    if (offset > piece_size || data.size() > piece_size - offset) {
        return false;
    }
    std::lock_guard lock(mutex_);
    auto& buffer = torrent.pieces_[piece];
    if (!buffer) {
        buffer = std::make_shared<std::vector<char>>(piece_size);
        used_bytes_ += piece_size;
        ++stored_pieces_;
    } else if (buffer->size() != piece_size) {
        return false;
    }
    if (!data.empty()) {
        std::memcpy(buffer->data() + offset, data.data(), data.size());
    }
    return true;
}

bool MemoryPieceStore::read(
    const Torrent& torrent,
    const std::uint32_t piece,
    const std::uint32_t offset,
    const std::span<char> destination
) const {
    std::lock_guard lock(mutex_);
    const auto found = torrent.pieces_.find(piece);
    if (found == torrent.pieces_.end()) {
        return false;
    }
    const auto& buffer = *found->second;
    if (offset > buffer.size() || destination.size() > buffer.size() - offset) {
        return false;
    }
    if (!destination.empty()) {
        std::memcpy(destination.data(), buffer.data() + offset, destination.size());
    }
    return true;
}

MemoryPieceStore::Piece MemoryPieceStore::piece(
    const Torrent& torrent,
    const std::uint32_t piece
) const {
    std::lock_guard lock(mutex_);
    const auto found = torrent.pieces_.find(piece);
    if (found == torrent.pieces_.end()) {
        return {};
    }
    return found->second;
}

void MemoryPieceStore::erase(Torrent& torrent, const std::uint32_t piece) {
    std::lock_guard lock(mutex_);
    erase_locked(torrent, piece);
}

void MemoryPieceStore::release(const std::string& key, const std::uint32_t piece) {
    std::lock_guard lock(mutex_);
    const auto registered = torrents_.find(key);
    if (registered == torrents_.end()) {
        return;
    }
    if (const auto torrent = registered->second.lock()) {
        erase_locked(*torrent, piece);
    }
}

std::uint64_t MemoryPieceStore::used_bytes() const {
    std::lock_guard lock(mutex_);
    return used_bytes_;
}

std::uint64_t MemoryPieceStore::stored_pieces() const {
    std::lock_guard lock(mutex_);
    return stored_pieces_;
}

void MemoryPieceStore::erase_locked(Torrent& torrent, const std::uint32_t piece) {
    const auto found = torrent.pieces_.find(piece);
    if (found == torrent.pieces_.end()) {
        return;
    }
    used_bytes_ -= found->second->size();
    --stored_pieces_;
    torrent.pieces_.erase(found);
}

}
