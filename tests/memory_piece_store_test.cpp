#include "test_support.hpp"

#include "storage/memory_piece_store.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace {

std::span<const char> text(const std::string& value) {
    return {value.data(), value.size()};
}

}

NUVIO_TEST("memory piece store reads back written blocks and accounts whole pieces") {
    nuvio::storage::MemoryPieceStore store;
    const auto torrent = store.attach("torrent");
    const std::string first = "abcd";
    const std::string second = "efgh";
    NUVIO_EXPECT_TRUE(store.write(*torrent, 3, 8, 0, text(first)));
    NUVIO_EXPECT_TRUE(store.write(*torrent, 3, 8, 4, text(second)));

    std::array<char, 6> destination{};
    NUVIO_EXPECT_TRUE(store.read(*torrent, 3, 2, destination));
    NUVIO_EXPECT_EQ(std::string(destination.begin(), destination.end()), std::string("cdefgh"));
    NUVIO_EXPECT_EQ(store.used_bytes(), std::uint64_t(8));
    NUVIO_EXPECT_EQ(store.stored_pieces(), std::uint64_t(1));
}

NUVIO_TEST("memory piece store rejects writes and reads outside the piece") {
    nuvio::storage::MemoryPieceStore store;
    const auto torrent = store.attach("torrent");
    const std::string data = "abcd";
    NUVIO_EXPECT_TRUE(!store.write(*torrent, 0, 6, 4, text(data)));
    NUVIO_EXPECT_TRUE(store.write(*torrent, 0, 6, 0, text(data)));
    NUVIO_EXPECT_TRUE(!store.write(*torrent, 0, 8, 0, text(data)));

    std::array<char, 4> destination{};
    NUVIO_EXPECT_TRUE(!store.read(*torrent, 0, 4, destination));
    NUVIO_EXPECT_TRUE(!store.read(*torrent, 1, 0, destination));
}

NUVIO_TEST("memory piece store releases pieces by torrent key") {
    nuvio::storage::MemoryPieceStore store;
    const auto torrent = store.attach("torrent");
    const std::string data = "abcd";
    NUVIO_EXPECT_TRUE(store.write(*torrent, 0, 4, 0, text(data)));
    NUVIO_EXPECT_TRUE(store.write(*torrent, 1, 4, 0, text(data)));
    const auto held = store.piece(*torrent, 0);

    store.release("torrent", 0);
    store.release("other", 1);

    NUVIO_EXPECT_TRUE(store.piece(*torrent, 0) == nullptr);
    NUVIO_EXPECT_TRUE(store.piece(*torrent, 1) != nullptr);
    NUVIO_EXPECT_EQ(std::string(held->begin(), held->end()), data);
    NUVIO_EXPECT_EQ(store.used_bytes(), std::uint64_t(4));
    NUVIO_EXPECT_EQ(store.stored_pieces(), std::uint64_t(1));
}

NUVIO_TEST("memory piece store keeps a re-added torrent when the old storage detaches") {
    nuvio::storage::MemoryPieceStore store;
    const auto old_torrent = store.attach("torrent");
    const std::string data = "abcd";
    NUVIO_EXPECT_TRUE(store.write(*old_torrent, 0, 4, 0, text(data)));
    const auto new_torrent = store.attach("torrent");
    NUVIO_EXPECT_TRUE(store.write(*new_torrent, 1, 4, 0, text(data)));

    store.detach(old_torrent);
    store.release("torrent", 1);

    NUVIO_EXPECT_TRUE(store.piece(*new_torrent, 1) == nullptr);
    NUVIO_EXPECT_EQ(store.used_bytes(), std::uint64_t(0));
    NUVIO_EXPECT_EQ(store.stored_pieces(), std::uint64_t(0));
}

NUVIO_TEST("memory piece store erases a failed piece and frees its bytes") {
    nuvio::storage::MemoryPieceStore store;
    const auto torrent = store.attach("torrent");
    const std::string data = "abcd";
    NUVIO_EXPECT_TRUE(store.write(*torrent, 2, 4, 0, text(data)));

    store.erase(*torrent, 2);

    NUVIO_EXPECT_TRUE(store.piece(*torrent, 2) == nullptr);
    NUVIO_EXPECT_EQ(store.used_bytes(), std::uint64_t(0));
}
